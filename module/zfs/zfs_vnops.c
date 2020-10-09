/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2018 by Delphix. All rights reserved.
 * Copyright (c) 2015 by Chunwei Chen. All rights reserved.
 * Copyright 2017 Nexenta Systems, Inc.
 */

/* Portions Copyright 2007 Jeremy Teo */
/* Portions Copyright 2010 Robert Milkowski */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <sys/uio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include <sys/errno.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_acl.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/dbuf.h>
#include <sys/policy.h>
#include <sys/zfs_vnops.h>
#include <sys/zfs_quota.h>
#include <sys/zfs_vfsops.h>
#include <sys/zfs_znode.h>


#define WANT_ASYNC

static ulong_t zfs_fsync_sync_cnt = 4;

int
zfs_fsync(znode_t *zp, int syncflag, cred_t *cr)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);

	(void) tsd_set(zfs_fsyncer_key, (void *)zfs_fsync_sync_cnt);

	if (zfsvfs->z_os->os_sync != ZFS_SYNC_DISABLED) {
		ZFS_ENTER(zfsvfs);
		ZFS_VERIFY_ZP(zp);
		zil_commit(zfsvfs->z_log, zp->z_id);
		ZFS_EXIT(zfsvfs);
	}
	tsd_set(zfs_fsyncer_key, NULL);

	return (0);
}

#if defined(SEEK_HOLE) && defined(SEEK_DATA)
/*
 * Lseek support for finding holes (cmd == SEEK_HOLE) and
 * data (cmd == SEEK_DATA). "off" is an in/out parameter.
 */
static int
zfs_holey_common(znode_t *zp, ulong_t cmd, loff_t *off)
{
	uint64_t noff = (uint64_t)*off; /* new offset */
	uint64_t file_sz;
	int error;
	boolean_t hole;

	file_sz = zp->z_size;
	if (noff >= file_sz)  {
		return (SET_ERROR(ENXIO));
	}

	if (cmd == F_SEEK_HOLE)
		hole = B_TRUE;
	else
		hole = B_FALSE;

	error = dmu_offset_next(ZTOZSB(zp)->z_os, zp->z_id, hole, &noff);

	if (error == ESRCH)
		return (SET_ERROR(ENXIO));

	/* file was dirty, so fall back to using generic logic */
	if (error == EBUSY) {
		if (hole)
			*off = file_sz;

		return (0);
	}

	/*
	 * We could find a hole that begins after the logical end-of-file,
	 * because dmu_offset_next() only works on whole blocks.  If the
	 * EOF falls mid-block, then indicate that the "virtual hole"
	 * at the end of the file begins at the logical EOF, rather than
	 * at the end of the last block.
	 */
	if (noff > file_sz) {
		ASSERT(hole);
		noff = file_sz;
	}

	if (noff < *off)
		return (error);
	*off = noff;
	return (error);
}

int
zfs_holey(znode_t *zp, ulong_t cmd, loff_t *off)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	ZFS_ENTER(zfsvfs);
	ZFS_VERIFY_ZP(zp);

	error = zfs_holey_common(zp, cmd, off);

	ZFS_EXIT(zfsvfs);
	return (error);
}
#endif /* SEEK_HOLE && SEEK_DATA */

/*ARGSUSED*/
int
zfs_access(znode_t *zp, int mode, int flag, cred_t *cr)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	ZFS_ENTER(zfsvfs);
	ZFS_VERIFY_ZP(zp);

	if (flag & V_ACE_MASK)
		error = zfs_zaccess(zp, mode, flag, B_FALSE, cr);
	else
		error = zfs_zaccess_rwx(zp, mode, flag, cr);

	ZFS_EXIT(zfsvfs);
	return (error);
}

static int
zfs_read_prologue(znode_t *zp, off_t offset, ssize_t resid)
{
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;

	ZFS_ENTER(zfsvfs);
	ZFS_VERIFY_ZP(zp);

	/* We don't copy out anything useful for directories. */
	if (Z_ISDIR(ZTOTYPE(zp))) {
		ZFS_EXIT(zfsvfs);
		return (SET_ERROR(EISDIR));
	}

	if (zp->z_pflags & ZFS_AV_QUARANTINED) {
		ZFS_EXIT(zfsvfs);
		return (SET_ERROR(EACCES));
	}

	/*
	 * Validate file offset
	 */
	if (offset < (offset_t)0) {
		ZFS_EXIT(zfsvfs);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Fasttrack empty reads
	 */
	if (resid == 0) {
		ZFS_EXIT(zfsvfs);
		return (0);
	}

	return (-1);
}

static unsigned long zfs_vnops_read_chunk_size = 1024 * 1024; /* Tunable */

/*
 * Read bytes from specified file into supplied buffer.
 *
 *	IN:	zp	- inode of file to be read from.
 *		uio	- structure supplying read location, range info,
 *			  and return buffer.
 *		ioflag	- O_SYNC flags; used to provide FRSYNC semantics.
 *			  O_DIRECT flag; used to bypass page cache.
 *		cr	- credentials of caller.
 *
 *	OUT:	uio	- updated offset and range, buffer filled.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Side Effects:
 *	inode - atime updated if byte count > 0
 */
/* ARGSUSED */
int
zfs_read(struct znode *zp, uio_t *uio, int ioflag, cred_t *cr)
{
	int error = 0;
	boolean_t frsync = B_FALSE;

	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	ZFS_ENTER(zfsvfs);
	ZFS_VERIFY_ZP(zp);

	error = zfs_read_prologue(zp, uio->uio_loffset, uio->uio_resid);
	if (error >= 0)
		return (error);

#ifdef FRSYNC
	/*
	 * If we're in FRSYNC mode, sync out this znode before reading it.
	 * Only do this for non-snapshots.
	 *
	 * Some platforms do not support FRSYNC and instead map it
	 * to O_SYNC, which results in unnecessary calls to zil_commit. We
	 * only honor FRSYNC requests on platforms which support it.
	 */
	frsync = !!(ioflag & FRSYNC);
#endif
	if (zfsvfs->z_log &&
	    (frsync || zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS))
		zil_commit(zfsvfs->z_log, zp->z_id);

	/*
	 * Lock the range against changes.
	 */
	zfs_locked_range_t *lr = zfs_rangelock_enter(&zp->z_rangelock,
	    uio->uio_loffset, uio->uio_resid, RL_READER);

	/*
	 * If we are reading past end-of-file we can skip
	 * to the end; but we might still need to set atime.
	 */
	if (uio->uio_loffset >= zp->z_size) {
		error = 0;
		goto out;
	}

	ASSERT(uio->uio_loffset < zp->z_size);
	ssize_t n = MIN(uio->uio_resid, zp->z_size - uio->uio_loffset);
	ssize_t start_resid = n;

	while (n > 0) {
		ssize_t nbytes = MIN(n, zfs_vnops_read_chunk_size -
		    P2PHASE(uio->uio_loffset, zfs_vnops_read_chunk_size));
#ifdef UIO_NOCOPY
		if (uio->uio_segflg == UIO_NOCOPY)
			error = mappedread_sf(zp, nbytes, uio);
		else
#endif
		if (zn_has_cached_data(zp) && !(ioflag & O_DIRECT)) {
			error = mappedread(zp, nbytes, uio);
		} else {
			error = dmu_read_uio_dbuf(sa_get_db(zp->z_sa_hdl),
			    uio, nbytes);
		}

		if (error) {
			/* convert checksum errors into IO errors */
			if (error == ECKSUM)
				error = SET_ERROR(EIO);
			break;
		}

		n -= nbytes;
	}

	int64_t nread = start_resid - n;
	dataset_kstats_update_read_kstats(&zfsvfs->z_kstat, nread);
	task_io_account_read(nread);
out:
	zfs_rangelock_exit(lr);

	ZFS_ACCESSTIME_STAMP(zfsvfs, zp);
	ZFS_EXIT(zfsvfs);
	return (error);
}

static int
zfs_write_prologue(znode_t *zp, ssize_t resid, off_t offset, int ioflag)
{
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	offset_t	woff;

	if (resid == 0)
		return (0);

	ZFS_ENTER(zfsvfs);
	ZFS_VERIFY_ZP(zp);
	/*
	 * Callers might not be able to detect properly that we are read-only,
	 * so check it explicitly here.
	 */
	if (zfs_is_readonly(zfsvfs)) {
		ZFS_EXIT(zfsvfs);
		return (SET_ERROR(EROFS));
	}

	/*
	 * If immutable or not appending then return EPERM.
	 * Intentionally allow ZFS_READONLY through here.
	 * See zfs_zaccess_common()
	 */
	if ((zp->z_pflags & ZFS_IMMUTABLE) ||
	    ((zp->z_pflags & ZFS_APPENDONLY) && !(ioflag & O_APPEND) &&
	    (offset < zp->z_size))) {
		ZFS_EXIT(zfsvfs);
		return (SET_ERROR(EPERM));
	}

	/*
	 * Validate file offset
	 */
	woff = ioflag & O_APPEND ? zp->z_size : offset;
	if (woff < 0) {
		ZFS_EXIT(zfsvfs);
		return (SET_ERROR(EINVAL));
	}

	return (-1);
}

/*
 * Write the bytes to a file.
 *
 *	IN:	zp	- znode of file to be written to.
 *		uio	- structure supplying write location, range info,
 *			  and data buffer.
 *		ioflag	- O_APPEND flag set if in append mode.
 *			  O_DIRECT flag; used to bypass page cache.
 *		cr	- credentials of caller.
 *
 *	OUT:	uio	- updated offset and range.
 *
 *	RETURN:	0 if success
 *		error code if failure
 *
 * Timestamps:
 *	ip - ctime|mtime updated if byte count > 0
 */

/* ARGSUSED */
int
zfs_write(znode_t *zp, uio_t *uio, int ioflag, cred_t *cr)
{
	int error = 0;
	ssize_t start_resid = uio->uio_resid;

	/*
	 * Fasttrack empty write
	 */
	ssize_t n = start_resid;
	if ((error = zfs_write_prologue(zp, start_resid, uio->uio_loffset,
	    ioflag)) >= 0)
		return (error);


	rlim64_t limit = MAXOFFSET_T;

	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	ZFS_ENTER(zfsvfs);
	ZFS_VERIFY_ZP(zp);

	sa_bulk_attr_t bulk[4];
	int count = 0;
	uint64_t mtime[2], ctime[2];
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL, &mtime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL, &ctime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_SIZE(zfsvfs), NULL,
	    &zp->z_size, 8);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, 8);


	offset_t woff = ioflag & O_APPEND ? zp->z_size : uio->uio_loffset;
	int max_blksz = zfsvfs->z_max_blksz;

	/*
	 * Pre-fault the pages to ensure slow (eg NFS) pages
	 * don't hold up txg.
	 * Skip this if uio contains loaned arc_buf.
	 */
	if (uio_prefaultpages(MIN(n, max_blksz), uio)) {
		ZFS_EXIT(zfsvfs);
		return (SET_ERROR(EFAULT));
	}

	/*
	 * If in append mode, set the io offset pointer to eof.
	 */
	zfs_locked_range_t *lr;
	if (ioflag & O_APPEND) {
		/*
		 * Obtain an appending range lock to guarantee file append
		 * semantics.  We reset the write offset once we have the lock.
		 */
		lr = zfs_rangelock_enter(&zp->z_rangelock, 0, n, RL_APPEND);
		woff = lr->lr_offset;
		if (lr->lr_length == UINT64_MAX) {
			/*
			 * We overlocked the file because this write will cause
			 * the file block size to increase.
			 * Note that zp_size cannot change with this lock held.
			 */
			woff = zp->z_size;
		}
		uio->uio_loffset = woff;
	} else {
		/*
		 * Note that if the file block size will change as a result of
		 * this write, then this range lock will lock the entire file
		 * so that we can re-write the block safely.
		 */
		lr = zfs_rangelock_enter(&zp->z_rangelock, woff, n, RL_WRITER);
	}

	if (zn_rlimit_fsize(zp, uio, uio->uio_td)) {
		zfs_rangelock_exit(lr);
		ZFS_EXIT(zfsvfs);
		return (EFBIG);
	}

	if (woff >= limit) {
		zfs_rangelock_exit(lr);
		ZFS_EXIT(zfsvfs);
		return (SET_ERROR(EFBIG));
	}

	if ((woff + n) > limit || woff > (limit - n))
		n = limit - woff;

	uint64_t end_size = MAX(zp->z_size, woff + n);
	zilog_t *zilog = zfsvfs->z_log;

	/*
	 * Write the file in reasonable size chunks.  Each chunk is written
	 * in a separate transaction; this keeps the intent log records small
	 * and allows us to do more fine-grained space accounting.
	 */
	while (n > 0) {
		woff = uio->uio_loffset;

		if (zfs_id_overblockquota(zfsvfs, DMU_USERUSED_OBJECT,
		    KUID_TO_SUID(ZTOUID(zp))) ||
		    zfs_id_overblockquota(zfsvfs, DMU_GROUPUSED_OBJECT,
		    KGID_TO_SGID(ZTOGID(zp))) ||
		    (zp->z_projid != ZFS_DEFAULT_PROJID &&
		    zfs_id_overblockquota(zfsvfs, DMU_PROJECTUSED_OBJECT,
		    zp->z_projid))) {
			error = SET_ERROR(EDQUOT);
			break;
		}

		arc_buf_t *abuf = NULL;
		if (n >= max_blksz && woff >= zp->z_size &&
		    P2PHASE(woff, max_blksz) == 0 &&
		    zp->z_blksz == max_blksz) {
			/*
			 * This write covers a full block.  "Borrow" a buffer
			 * from the dmu so that we can fill it before we enter
			 * a transaction.  This avoids the possibility of
			 * holding up the transaction if the data copy hangs
			 * up on a pagefault (e.g., from an NFS server mapping).
			 */
			size_t cbytes;

			abuf = dmu_request_arcbuf(sa_get_db(zp->z_sa_hdl),
			    max_blksz);
			ASSERT(abuf != NULL);
			ASSERT(arc_buf_size(abuf) == max_blksz);
			if ((error = uiocopy(abuf->b_data, max_blksz,
			    UIO_WRITE, uio, &cbytes))) {
				dmu_return_arcbuf(abuf);
				break;
			}
			ASSERT(cbytes == max_blksz);
		}

		/*
		 * Start a transaction.
		 */
		dmu_tx_t *tx = dmu_tx_create(zfsvfs->z_os);
		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
		dmu_buf_impl_t *db = (dmu_buf_impl_t *)sa_get_db(zp->z_sa_hdl);
		DB_DNODE_ENTER(db);
		dmu_tx_hold_write_by_dnode(tx, DB_DNODE(db), woff,
		    MIN(n, max_blksz));
		DB_DNODE_EXIT(db);
		zfs_sa_upgrade_txholds(tx, zp);
		error = dmu_tx_assign(tx, TXG_WAIT);
		if (error) {
			dmu_tx_abort(tx);
			if (abuf != NULL)
				dmu_return_arcbuf(abuf);
			break;
		}

		/*
		 * If rangelock_enter() over-locked we grow the blocksize
		 * and then reduce the lock range.  This will only happen
		 * on the first iteration since rangelock_reduce() will
		 * shrink down lr_length to the appropriate size.
		 */
		if (lr->lr_length == UINT64_MAX) {
			uint64_t new_blksz;

			if (zp->z_blksz > max_blksz) {
				/*
				 * File's blocksize is already larger than the
				 * "recordsize" property.  Only let it grow to
				 * the next power of 2.
				 */
				ASSERT(!ISP2(zp->z_blksz));
				new_blksz = MIN(end_size,
				    1 << highbit64(zp->z_blksz));
			} else {
				new_blksz = MIN(end_size, max_blksz);
			}
			zfs_grow_blocksize(zp, new_blksz, tx);
			zfs_rangelock_reduce(lr, woff, n);
		}

		/*
		 * XXX - should we really limit each write to z_max_blksz?
		 * Perhaps we should use SPA_MAXBLOCKSIZE chunks?
		 */
		ssize_t nbytes = MIN(n, max_blksz - P2PHASE(woff, max_blksz));

		ssize_t tx_bytes;
		if (abuf == NULL) {
			tx_bytes = uio->uio_resid;
			uio_fault_disable(uio, B_TRUE);
			error = dmu_write_uio_dbuf(sa_get_db(zp->z_sa_hdl),
			    uio, nbytes, tx);
			uio_fault_disable(uio, B_FALSE);
#ifdef __linux__
			if (error == EFAULT) {
				dmu_tx_commit(tx);
				/*
				 * Account for partial writes before
				 * continuing the loop.
				 * Update needs to occur before the next
				 * uio_prefaultpages, or prefaultpages may
				 * error, and we may break the loop early.
				 */
				if (tx_bytes != uio->uio_resid)
					n -= tx_bytes - uio->uio_resid;
				if (uio_prefaultpages(MIN(n, max_blksz), uio)) {
					break;
				}
				continue;
			}
#endif
			if (error != 0) {
				dmu_tx_commit(tx);
				break;
			}
			tx_bytes -= uio->uio_resid;
		} else {
			/*
			 * Is this block ever reached?
			 */
			tx_bytes = nbytes;
			/*
			 * If this is not a full block write, but we are
			 * extending the file past EOF and this data starts
			 * block-aligned, use assign_arcbuf().  Otherwise,
			 * write via dmu_write().
			 */

			if (tx_bytes == max_blksz) {
				error = dmu_assign_arcbuf_by_dbuf(
				    sa_get_db(zp->z_sa_hdl), woff, abuf, tx);
				if (error != 0) {
					dmu_return_arcbuf(abuf);
					dmu_tx_commit(tx);
					break;
				}
			}
			ASSERT(tx_bytes <= uio->uio_resid);
			uioskip(uio, tx_bytes);
		}
		if (tx_bytes && zn_has_cached_data(zp) &&
		    !(ioflag & O_DIRECT)) {
			update_pages(zp, woff,
			    tx_bytes, zfsvfs->z_os, zp->z_id);
		}

		/*
		 * If we made no progress, we're done.  If we made even
		 * partial progress, update the znode and ZIL accordingly.
		 */
		if (tx_bytes == 0) {
			(void) sa_update(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
			    (void *)&zp->z_size, sizeof (uint64_t), tx);
			dmu_tx_commit(tx);
			ASSERT(error != 0);
			break;
		}

		/*
		 * Clear Set-UID/Set-GID bits on successful write if not
		 * privileged and at least one of the execute bits is set.
		 *
		 * It would be nice to do this after all writes have
		 * been done, but that would still expose the ISUID/ISGID
		 * to another app after the partial write is committed.
		 *
		 * Note: we don't call zfs_fuid_map_id() here because
		 * user 0 is not an ephemeral uid.
		 */
		mutex_enter(&zp->z_acl_lock);
		uint32_t uid = KUID_TO_SUID(ZTOUID(zp));
		if ((zp->z_mode & (S_IXUSR | (S_IXUSR >> 3) |
		    (S_IXUSR >> 6))) != 0 &&
		    (zp->z_mode & (S_ISUID | S_ISGID)) != 0 &&
		    secpolicy_vnode_setid_retain(zp, cr,
		    ((zp->z_mode & S_ISUID) != 0 && uid == 0)) != 0) {
			uint64_t newmode;
			zp->z_mode &= ~(S_ISUID | S_ISGID);
			(void) sa_update(zp->z_sa_hdl, SA_ZPL_MODE(zfsvfs),
			    (void *)&newmode, sizeof (uint64_t), tx);
		}
		mutex_exit(&zp->z_acl_lock);

		zfs_tstamp_update_setup(zp, CONTENT_MODIFIED, mtime, ctime);

		/*
		 * Update the file size (zp_size) if it has changed;
		 * account for possible concurrent updates.
		 */
		while ((end_size = zp->z_size) < uio->uio_loffset) {
			(void) atomic_cas_64(&zp->z_size, end_size,
			    uio->uio_loffset);
			ASSERT(error == 0);
		}
		/*
		 * If we are replaying and eof is non zero then force
		 * the file size to the specified eof. Note, there's no
		 * concurrency during replay.
		 */
		if (zfsvfs->z_replay && zfsvfs->z_replay_eof != 0)
			zp->z_size = zfsvfs->z_replay_eof;

		error = sa_bulk_update(zp->z_sa_hdl, bulk, count, tx);

		zfs_log_write(zilog, tx, TX_WRITE, zp, woff, tx_bytes, ioflag,
		    NULL, NULL);
		dmu_tx_commit(tx);

		if (error != 0)
			break;
		ASSERT(tx_bytes == nbytes);
		n -= nbytes;

		if (n > 0) {
			if (uio_prefaultpages(MIN(n, max_blksz), uio)) {
				error = EFAULT;
				break;
			}
		}
	}

	zfs_inode_update(zp);
	zfs_rangelock_exit(lr);

	/*
	 * If we're in replay mode, or we made no progress, return error.
	 * Otherwise, it's at least a partial write, so it's successful.
	 */
	if (zfsvfs->z_replay || uio->uio_resid == start_resid) {
		ZFS_EXIT(zfsvfs);
		return (error);
	}

	if (ioflag & (O_SYNC | O_DSYNC) ||
	    zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, zp->z_id);

	int64_t nwritten = start_resid - uio->uio_resid;
	dataset_kstats_update_write_kstats(&zfsvfs->z_kstat, nwritten);
	task_io_account_write(nwritten);

	ZFS_EXIT(zfsvfs);
	return (0);
}

/*ARGSUSED*/
int
zfs_getsecattr(znode_t *zp, vsecattr_t *vsecp, int flag, cred_t *cr)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;
	boolean_t skipaclchk = (flag & ATTR_NOACLCHECK) ? B_TRUE : B_FALSE;

	ZFS_ENTER(zfsvfs);
	ZFS_VERIFY_ZP(zp);
	error = zfs_getacl(zp, vsecp, skipaclchk, cr);
	ZFS_EXIT(zfsvfs);

	return (error);
}

/*ARGSUSED*/
int
zfs_setsecattr(znode_t *zp, vsecattr_t *vsecp, int flag, cred_t *cr)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;
	boolean_t skipaclchk = (flag & ATTR_NOACLCHECK) ? B_TRUE : B_FALSE;
	zilog_t	*zilog = zfsvfs->z_log;

	ZFS_ENTER(zfsvfs);
	ZFS_VERIFY_ZP(zp);

	error = zfs_setacl(zp, vsecp, skipaclchk, cr);

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	ZFS_EXIT(zfsvfs);
	return (error);
}
#ifdef WANT_ASYNC

uint64_t
dmu_physmove(dmu_buf_set_t *dbs, dmu_buf_t *db, uint64_t off, uint64_t sz)
{
	struct uio_bio *uio = (struct uio_bio *)dbs->dbs_dc->dc_data_buf;
	uint64_t adv = uio->uio_resid;
	int err;

	err = uiobiomove((char *)db->db_data + off, sz, uio);
	if (err)
		dbs->dbs_err = err;
	adv -= uio->uio_resid;

	return (adv);
}

static boolean_t
dnode_has_dirty(dnode_t *dn)
{
	boolean_t dirty = B_FALSE;

	/*
	 * Check if dnode is dirty
	 */
	for (int i = 0; i < TXG_SIZE; i++) {
		if (multilist_link_active(&dn->dn_dirty_link[i])) {
			dirty = B_TRUE;
			break;
		}
	}
	return (dirty);
}

void
zfs_read_async_epilogue(zfs_read_state_t *state, int error)
{
	znode_t		*zp = state->zrs_zp;
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	struct uio_bio *uio = state->zrs_uio;
	dmu_buf_impl_t *db = (dmu_buf_impl_t *)state->zrs_db;

	DB_DNODE_EXIT(db);
	zfs_rangelock_exit(state->zrs_lr);
	ZFS_ACCESSTIME_STAMP(zfsvfs, zp);
	ZFS_EXIT(zfsvfs);
	if (error) {
		uio->uio_flags |= UIO_BIO_ERROR;
		uio->uio_error = error;
	}
	uio->uio_bio_done(uio);
	kmem_free(state, sizeof (*state));
}

static void
zfs_read_async_resume(void *arg)
{
	zfs_read_state_t *state = arg;
	znode_t		*zp = state->zrs_zp;
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	struct uio_bio *uio = state->zrs_uio;
	dnode_t *dn = state->zrs_dn;
	zfs_locked_range_t		*lr;
	int flags, error = 0;

	/*
	 * Lock the range against changes.
	 */
	if ((state->zrs_done & ZRS_RANGELOCK) == 0) {
		state->zrs_done |= ZRS_RANGELOCK;
		error = zfs_rangelock_tryenter_async(&zp->z_rangelock,
		    uio->uio_loffset, uio->uio_resid, RL_READER, &state->zrs_lr,
		    zfs_read_async_resume, state);
		if (error == EINPROGRESS)
			return;
		VERIFY(error == 0);
	}
	lr = state->zrs_lr;
	/*
	 * If we are reading past end-of-file we can skip
	 * to the end; but we might still need to set atime.
	 */
	if (uio->uio_loffset >= zp->z_size) {
		error = 0;
		goto out;
	}
	if (zp_has_cached_in_range(zp, uio->uio_loffset, uio->uio_resid)) {
		zfs_mappedread_async(state);
		return;
	}
	flags = DMU_CTX_FLAG_READ | DMU_CTX_FLAG_ASYNC |
	    DMU_CTX_FLAG_NO_HOLD | DMU_CTX_FLAG_PREFETCH;
	if ((state->zrs_done & ZRS_DMU_ISSUED) == 0) {
		state->zrs_done |= ZRS_DMU_ISSUED;
		error = dmu_ctx_init(&state->zrs_dc, dn, zfsvfs->z_os, zp->z_id,
		    uio->uio_loffset, uio->uio_resid, uio, FTAG, flags);
		if (error)
			goto out;
		state->zrs_dc.dc_data_transfer_cb = dmu_physmove;
		dmu_ctx_set_complete_cb(&state->zrs_dc,
		    (dmu_ctx_cb_t)zfs_read_async_resume);
		error = dmu_issue(&state->zrs_dc);
		dmu_ctx_rele(&state->zrs_dc);
		if (error == 0 || error == EINPROGRESS)
			return;
	}
out:
	zfs_read_async_epilogue(state, error);
}

int
zfs_read_async(znode_t *zp, struct uio_bio *uio, int ioflag)
{
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	int		error = 0;
	dmu_buf_impl_t *db;
	dnode_t *dn;
	zfs_read_state_t		*state;

	error = zfs_read_prologue(zp, uio->uio_loffset, uio->uio_resid);
	if (error >= 0)
		return (error);

	state = kmem_zalloc(sizeof (*state), KM_SLEEP);
	state->zrs_zp = zp;
	state->zrs_uio = uio;
	state->zrs_ioflag = ioflag;

	state->zrs_db = sa_get_db(zp->z_sa_hdl);
	db = (dmu_buf_impl_t *)state->zrs_db;
	DB_DNODE_ENTER(db);
	state->zrs_dn = dn = DB_DNODE(db);
	/*
	 * If we're in FRSYNC mode, sync out this znode before reading it.
	 */
	if (zfsvfs->z_log &&
	    (ioflag & FRSYNC || zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS) &&
	    dnode_has_dirty(dn))
		error = zil_commit_async(zfsvfs->z_log, zp->z_id,
		    zfs_read_async_resume, state);

	if (error >= 0) {
		ASSERT(error == EINPROGRESS);
		return (error);
	}
	zfs_read_async_resume(state);
	return (EINPROGRESS);
}

typedef enum {
	ZWS_BLOCKED	= 1 << 1,
	ZWS_RANGELOCK_PRE	= 1 << 2,
	ZWS_RANGELOCK_POST	= 1 << 3,
	ZWS_TX_ASSIGNED	= 1 << 4,
	ZWS_DMU_ISSUED	= 1 << 5,
	ZWS_TX_BYTES_UPDATED	= 1 << 6,
	ZWS_UPDATED_PAGES	= 1 << 7,
} zws_done_t;


typedef struct zfs_write_state {
	dmu_ctx_t	zws_dc;
	znode_t	*zws_zp;
	dnode_t	*zws_dn;
	dmu_buf_impl_t	*zws_db;
	zfs_locked_range_t	*zws_lr;
	struct uio_bio	*zws_uio;
	struct ucred *zws_cred;
	dmu_tx_t	*zws_tx;
	sa_bulk_attr_t	zws_bulk[4];
	uint64_t	zws_mtime[2];
	uint64_t zws_ctime[2];
	int zws_ioflag;
	int zws_tx_bytes;
	uint16_t zws_done;
	uint8_t zws_rc;
} zfs_write_state_t;

static void zfs_write_async_resume(zfs_write_state_t *state);

static int
zfs_rangelock_write_async(zfs_write_state_t *state)
{
	int error;
	off_t range_off;
	ssize_t woff, range_len;
	zfs_rangelock_type_t type;
	zfs_locked_range_t *lr;
	znode_t *zp = state->zws_zp;
	struct uio_bio *uio = state->zws_uio;
	struct thread *td = uio->uio_td;

	woff = state->zws_uio->uio_loffset;
	range_len = state->zws_uio->uio_resid;
	range_off = (state->zws_ioflag & FAPPEND) ? 0 : woff;
	type = (state->zws_ioflag & FAPPEND) ? RL_APPEND : RL_WRITER;

	if ((state->zws_done & ZWS_RANGELOCK_PRE) == 0) {
		state->zws_done |= ZWS_RANGELOCK_PRE;
		error = zfs_rangelock_tryenter_async(&zp->z_rangelock,
		    range_off, range_len, type, &state->zws_lr,
		    (callback_fn)zfs_write_async_resume, state);
		if (error == EINPROGRESS) {
			state->zws_done |= ZWS_BLOCKED;
			return (error);
		}
	}
	lr = state->zws_lr;
	if (state->zws_ioflag & FAPPEND) {
		woff = lr->lr_offset;
		if (lr->lr_length == UINT64_MAX) {
			/*
			 * We overlocked the file because this write will cause
			 * the file block size to increase.
			 * Note that zp_size cannot change with this lock held.
			 */
			woff = zp->z_size;
		}
		uio->uio_loffset = woff;
	}
	if (woff > MAXOFFSET_T)
		return (EFBIG);
#ifdef RLIMIT_FSIZE
	if (uio->uio_loffset + uio->uio_resid > lim_cur(td, RLIMIT_FSIZE)) {
		PROC_LOCK(td->td_proc);
		kern_psignal(td->td_proc, SIGXFSZ);
		PROC_UNLOCK(td->td_proc);
	}
#endif
	state->zws_done |= ZWS_RANGELOCK_POST;
	return (0);
}

static void
zfs_write_async_epilogue(zfs_write_state_t *state)
{
	int rc = state->zws_rc;
	struct uio_bio *uio = state->zws_uio;

	if ((state->zws_done & ZWS_BLOCKED) == 0)
		return;

	if (state->zws_lr)
		zfs_rangelock_exit(state->zws_lr);
	if (state->zws_dn)
		DB_DNODE_EXIT(state->zws_db);
	ZFS_EXIT(state->zws_zp->z_zfsvfs);

	if (rc && rc != EINPROGRESS) {
		uio->uio_flags |= UIO_BIO_ERROR;
		uio->uio_error = rc;
	}
	kmem_free(state, sizeof (*state));
	uio->uio_bio_done(uio);
}

static void
zfs_write_async_resume(zfs_write_state_t *state)
{
	znode_t		*zp = state->zws_zp;
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	struct uio_bio *uio = state->zws_uio;
	rlim64_t	limit = MAXOFFSET_T;
	int		max_blksz = zfsvfs->z_max_blksz;
	struct ucred *cr = state->zws_cred;
	zilog_t		*zilog;
	dmu_buf_impl_t *db;
	dnode_t *dn;
	zfs_locked_range_t		*lr;
	dmu_tx_t	*tx;
	uint64_t	end_size;
	uint64_t	uid, gid, projid;
	int		tx_bytes, flags, error, write_eof;
	off_t	woff;
	ssize_t n;
	int rc = 0;

	if ((state->zws_done & ZWS_RANGELOCK_POST) == 0)
		rc = zfs_rangelock_write_async(state);
	if (rc) {
		state->zws_rc = rc;
		goto done;
	}
	lr = state->zws_lr;
	woff = uio->uio_loffset;
	n = uio->uio_resid;
	if ((woff + n) > limit || woff > (limit - n))
		n = limit - woff;

	/* Will this write extend the file length? */
	write_eof = (woff + n > zp->z_size);

	end_size = MAX(zp->z_size, woff + n);
	uid = zp->z_uid;
	gid = zp->z_gid;
	projid = zp->z_projid;

	if (zfs_id_overblockquota(zfsvfs, DMU_USERUSED_OBJECT, uid) ||
	    zfs_id_overblockquota(zfsvfs, DMU_GROUPUSED_OBJECT, gid) ||
	    (projid != ZFS_DEFAULT_PROJID &&
	    zfs_id_overblockquota(zfsvfs, DMU_PROJECTUSED_OBJECT, projid))) {
		state->zws_rc = SET_ERROR(EDQUOT);
		goto done;
	}
	if ((state->zws_done & ZWS_TX_ASSIGNED) == 0) {
		state->zws_tx = tx =  dmu_tx_create(zfsvfs->z_os);
		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
		state->zws_db = db = (dmu_buf_impl_t *)sa_get_db(zp->z_sa_hdl);
		DB_DNODE_ENTER(db);
		state->zws_dn = dn = DB_DNODE(db);
		dmu_tx_hold_write_by_dnode(tx, dn, woff, n);
		/* XXX may do synchronous I/O if it has an external ACL */
		zfs_sa_upgrade_txholds(tx, zp);
		state->zws_done |= (ZWS_TX_ASSIGNED|ZWS_BLOCKED);
		error = dmu_tx_assign_async(tx,
		    (callback_fn)zfs_write_async_resume, state);
		if (error == EINPROGRESS)
			return;
		state->zws_done &= ~ZWS_BLOCKED;
		if (error) {
			state->zws_done &= ~ZWS_TX_ASSIGNED;
			state->zws_rc = error;
			goto done;
		}
	}
	tx = state->zws_tx;
	dn = state->zws_dn;
	/*
	 * If zfs_range_lock() over-locked we grow the blocksize
	 * and then reduce the lock range.  This will only happen
	 * on the first iteration since zfs_range_reduce() will
	 * shrink down r_len to the appropriate size.
	 */
	if (lr->lr_length == UINT64_MAX) {
		uint64_t new_blksz;

		if (zp->z_blksz > max_blksz) {
			/*
			 * File's blocksize is already larger than the
			 * "recordsize" property.  Only let it grow to
			 * the next power of 2.
			 */
			ASSERT(!ISP2(zp->z_blksz));
			new_blksz = MIN(end_size,
			    1 << highbit64(zp->z_blksz));
		} else {
			new_blksz = MIN(end_size, max_blksz);
		}
		zfs_grow_blocksize(zp, new_blksz, tx);
		zfs_rangelock_reduce(lr, woff, n);
	}
	if ((state->zws_done & ZWS_DMU_ISSUED) == 0) {
		state->zws_done |= ZWS_DMU_ISSUED;
		if (woff + n > zp->z_size)
			vnode_pager_setsize(ZTOV(zp), woff + n);
		flags = DMU_CTX_FLAG_ASYNC | DMU_CTX_FLAG_NO_HOLD;
		state->zws_tx_bytes = uio->uio_resid;
		error = dmu_ctx_init(&state->zws_dc, dn, zfsvfs->z_os, zp->z_id,
		    uio->uio_loffset, uio->uio_resid, uio, FTAG, flags);
		if (error)
			goto done;
		state->zws_dc.dc_data_transfer_cb = dmu_physmove;
		dmu_ctx_set_complete_cb(&state->zws_dc,
		    (dmu_ctx_cb_t)zfs_write_async_resume);
		dmu_ctx_set_dmu_tx(&state->zws_dc, tx);
		error = dmu_issue(&state->zws_dc);
		dmu_ctx_rele(&state->zws_dc);
		if (error && error != EINPROGRESS)
			goto done;
		state->zws_done |= ZWS_BLOCKED;
		return;
	}
	if ((state->zws_done & ZWS_TX_BYTES_UPDATED) == 0) {
		state->zws_done |= ZWS_TX_BYTES_UPDATED;
		state->zws_tx_bytes -= uio->uio_resid;
	}
	tx_bytes = state->zws_tx_bytes;
	error = state->zws_dc.dc_err;

	/*
	 * If we made no progress, we're done.  If we made even
	 * partial progress, update the znode and ZIL accordingly.
	 */
	if (tx_bytes == 0) {
		(void) sa_update(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
		    (void *)&zp->z_size, sizeof (uint64_t), tx);
		dmu_tx_commit(tx);
		ASSERT(error != 0);
		goto done;
	}
	if (zp_has_cached_in_range(zp, uio->uio_loffset, tx_bytes) &&
	    ((state->zws_done & ZWS_UPDATED_PAGES) == 0)) {
		state->zws_done |= ZWS_UPDATED_PAGES;
		error = update_pages_async(zp, woff, tx_bytes, dn,
		    zfsvfs->z_os, zp->z_id, (callback_fn)zfs_write_async_resume,
		    state);
		if (error == EINPROGRESS)
			return;
	}

	/*
	 * Clear Set-UID/Set-GID bits on successful write if not
	 * privileged and at least one of the excute bits is set.
	 *
	 * It would be nice to to this after all writes have
	 * been done, but that would still expose the ISUID/ISGID
	 * to another app after the partial write is committed.
	 *
	 * Note: we don't call zfs_fuid_map_id() here because
	 * user 0 is not an ephemeral uid.
	 */
	mutex_enter(&zp->z_acl_lock);
	if ((zp->z_mode & (S_IXUSR | (S_IXUSR >> 3) |
	    (S_IXUSR >> 6))) != 0 &&
	    (zp->z_mode & (S_ISUID | S_ISGID)) != 0 &&
	    secpolicy_vnode_setid_retain(zp, cr,
	    (zp->z_mode & S_ISUID) != 0 && zp->z_uid == 0) != 0) {
		uint64_t newmode;
		zp->z_mode &= ~(S_ISUID | S_ISGID);
		newmode = zp->z_mode;
		(void) sa_update(zp->z_sa_hdl, SA_ZPL_MODE(zfsvfs),
		    (void *)&newmode, sizeof (uint64_t), tx);
	}
	mutex_exit(&zp->z_acl_lock);

	zfs_tstamp_update_setup(zp, CONTENT_MODIFIED, state->zws_mtime,
	    state->zws_ctime);
		/*
		 * Update the file size (zp_size) if it has changed;
		 * account for possible concurrent updates.
		 */
	while ((end_size = zp->z_size) < uio->uio_loffset)
		atomic_cas_64(&zp->z_size, end_size, uio->uio_loffset);

	/*
	 * If we are replaying and eof is non zero then force
	 * the file size to the specified eof. Note, there's no
	 * concurrency during replay.
	 */
	if (zfsvfs->z_replay && zfsvfs->z_replay_eof != 0)
		zp->z_size = zfsvfs->z_replay_eof;

	if (error == 0)
		error = sa_bulk_update(zp->z_sa_hdl, state->zws_bulk, 4, tx);
	else
		(void) sa_bulk_update(zp->z_sa_hdl, state->zws_bulk, 4, tx);

	zilog = zfsvfs->z_log;
	zfs_log_write(zilog, tx, TX_WRITE, zp, woff, tx_bytes,
	    state->zws_ioflag, NULL, NULL);
	dmu_tx_commit(tx);
	if (error)
		goto done;

	if (state->zws_ioflag & (FSYNC | FDSYNC) ||
	    zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS) {
		error = zil_commit_async(zilog, zp->z_id,
		    (callback_fn)zfs_write_async_epilogue, state);
		if (error == EINPROGRESS)
			return;
	}
done:
	zfs_write_async_epilogue(state);
}

int
zfs_write_async(znode_t *zp, struct uio_bio *uio, int ioflag)
{
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	zfs_write_state_t *state;
	sa_bulk_attr_t *bulk;
	int rc, c;

	if ((rc = zfs_write_prologue(zp, uio->uio_resid, uio->uio_loffset,
	    ioflag)) >= 0)
		return (rc);

	state = kmem_zalloc(sizeof (*state), KM_SLEEP);
	state->zws_zp = zp;
	state->zws_uio = uio;
	state->zws_ioflag = ioflag;
	bulk = (void*)&state->zws_bulk;
	c = 0;
	SA_ADD_BULK_ATTR(bulk, c, SA_ZPL_MTIME(zfsvfs), NULL,
	    &state->zws_mtime, 16);
	SA_ADD_BULK_ATTR(bulk, c, SA_ZPL_CTIME(zfsvfs), NULL,
	    &state->zws_ctime, 16);
	SA_ADD_BULK_ATTR(bulk, c, SA_ZPL_SIZE(zfsvfs), NULL,
	    &zp->z_size, 8);
	SA_ADD_BULK_ATTR(bulk, c, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, 8);

	zfs_write_async_resume(state);
	rc = state->zws_rc;
	if (rc && rc != EINPROGRESS) {
		if (state->zws_lr)
			zfs_rangelock_exit(state->zws_lr);
		ZFS_EXIT(state->zws_zp->z_zfsvfs);
		kmem_free(state, sizeof (*state));
	}
	return (rc);
}

typedef struct zfs_sync_state {
	znode_t *zss_zp;
	struct uio_bio *zss_uio;
} zfs_sync_state_t;

static void
zfs_sync_async_done(void *arg)
{
	zfs_sync_state_t *zss = arg;
	struct uio_bio *uio;

	ZFS_EXIT(zss->zss_zp->z_zfsvfs);
	uio = zss->zss_uio;
	kmem_free(zss, sizeof (*zss));
	uio->uio_bio_done(uio);
}

int
zfs_sync_async(znode_t *zp, struct uio_bio *uio)
{
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	zfs_sync_state_t *zss;
	int		rc;

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_DISABLED)
		return (0);

	zss = kmem_alloc(sizeof (*zss), KM_SLEEP);
	zss->zss_zp = zp;
	zss->zss_uio = uio;
	ZFS_ENTER(zfsvfs);
	ZFS_VERIFY_ZP(zp);
	rc = zil_commit_async(zfsvfs->z_log, zp->z_id,
	    zfs_sync_async_done, zss);
	if (rc == 0) {
		ZFS_EXIT(zfsvfs);
		kmem_free(zss, sizeof (*zss));
	}
	return (rc);
}

int
zfs_ubop(znode_t *zp, struct uio_bio *uio, int ioflag)
{
	uint8_t cmd;
	int rc;

	cmd = uio->uio_cmd;
	switch (cmd) {
		case UIO_BIO_READ:
			rc = zfs_read_async(zp, uio, ioflag);
			break;
		case UIO_BIO_WRITE:
			rc = zfs_write_async(zp, uio, ioflag);
			break;
		case UIO_BIO_SYNC:
			rc = zfs_sync_async(zp, uio);
			break;
		default:
			rc = EOPNOTSUPP;
	}
	return (rc);
}



#endif


#ifdef ZFS_DEBUG
static int zil_fault_io = 0;
#endif

static void zfs_get_done(zgd_t *zgd, int error);

/*
 * Get data to generate a TX_WRITE intent log record.
 */
int
zfs_get_data(void *arg, lr_write_t *lr, char *buf, struct lwb *lwb, zio_t *zio)
{
	zfsvfs_t *zfsvfs = arg;
	objset_t *os = zfsvfs->z_os;
	znode_t *zp;
	uint64_t object = lr->lr_foid;
	uint64_t offset = lr->lr_offset;
	uint64_t size = lr->lr_length;
	dmu_buf_t *db;
	zgd_t *zgd;
	int error = 0;

	ASSERT3P(lwb, !=, NULL);
	ASSERT3P(zio, !=, NULL);
	ASSERT3U(size, !=, 0);

	/*
	 * Nothing to do if the file has been removed
	 */
	if (zfs_zget(zfsvfs, object, &zp) != 0)
		return (SET_ERROR(ENOENT));
	if (zp->z_unlinked) {
		/*
		 * Release the vnode asynchronously as we currently have the
		 * txg stopped from syncing.
		 */
		zfs_zrele_async(zp);
		return (SET_ERROR(ENOENT));
	}

	zgd = (zgd_t *)kmem_zalloc(sizeof (zgd_t), KM_SLEEP);
	zgd->zgd_lwb = lwb;
	zgd->zgd_private = zp;

	/*
	 * Write records come in two flavors: immediate and indirect.
	 * For small writes it's cheaper to store the data with the
	 * log record (immediate); for large writes it's cheaper to
	 * sync the data and get a pointer to it (indirect) so that
	 * we don't have to write the data twice.
	 */
	if (buf != NULL) { /* immediate write */
		zgd->zgd_lr = zfs_rangelock_enter(&zp->z_rangelock,
		    offset, size, RL_READER);
		/* test for truncation needs to be done while range locked */
		if (offset >= zp->z_size) {
			error = SET_ERROR(ENOENT);
		} else {
			error = dmu_read(os, object, offset, size, buf,
			    0 /* flags */);
		}
		ASSERT(error == 0 || error == ENOENT);
	} else { /* indirect write */
		/*
		 * Have to lock the whole block to ensure when it's
		 * written out and its checksum is being calculated
		 * that no one can change the data. We need to re-check
		 * blocksize after we get the lock in case it's changed!
		 */
		for (;;) {
			uint64_t blkoff;
			size = zp->z_blksz;
			blkoff = ISP2(size) ? P2PHASE(offset, size) : offset;
			offset -= blkoff;
			zgd->zgd_lr = zfs_rangelock_enter(&zp->z_rangelock,
			    offset, size, RL_READER);
			if (zp->z_blksz == size)
				break;
			offset += blkoff;
			zfs_rangelock_exit(zgd->zgd_lr);
		}
		/* test for truncation needs to be done while range locked */
		if (lr->lr_offset >= zp->z_size)
			error = SET_ERROR(ENOENT);
#ifdef ZFS_DEBUG
		if (zil_fault_io) {
			error = SET_ERROR(EIO);
			zil_fault_io = 0;
		}
#endif
		if (error == 0)
			error = dmu_buf_hold(os, object, offset, zgd, &db,
			    0 /* flags */);

		if (error == 0) {
			blkptr_t *bp = &lr->lr_blkptr;

			zgd->zgd_db = db;
			zgd->zgd_bp = bp;

			ASSERT(db->db_offset == offset);
			ASSERT(db->db_size == size);

			error = dmu_sync(zio, lr->lr_common.lrc_txg,
			    zfs_get_done, zgd);
			ASSERT(error || lr->lr_length <= size);

			/*
			 * On success, we need to wait for the write I/O
			 * initiated by dmu_sync() to complete before we can
			 * release this dbuf.  We will finish everything up
			 * in the zfs_get_done() callback.
			 */
			if (error == 0)
				return (0);

			if (error == EALREADY) {
				lr->lr_common.lrc_txtype = TX_WRITE2;
				/*
				 * TX_WRITE2 relies on the data previously
				 * written by the TX_WRITE that caused
				 * EALREADY.  We zero out the BP because
				 * it is the old, currently-on-disk BP.
				 */
				zgd->zgd_bp = NULL;
				BP_ZERO(bp);
				error = 0;
			}
		}
	}

	zfs_get_done(zgd, error);

	return (error);
}


/* ARGSUSED */
static void
zfs_get_done(zgd_t *zgd, int error)
{
	znode_t *zp = zgd->zgd_private;

	if (zgd->zgd_db)
		dmu_buf_rele(zgd->zgd_db, zgd);

	zfs_rangelock_exit(zgd->zgd_lr);

	/*
	 * Release the vnode asynchronously as we currently have the
	 * txg stopped from syncing.
	 */
	zfs_zrele_async(zp);

	kmem_free(zgd, sizeof (zgd_t));
}

EXPORT_SYMBOL(zfs_access);
EXPORT_SYMBOL(zfs_fsync);
EXPORT_SYMBOL(zfs_holey);
EXPORT_SYMBOL(zfs_read);
EXPORT_SYMBOL(zfs_write);
EXPORT_SYMBOL(zfs_getsecattr);
EXPORT_SYMBOL(zfs_setsecattr);

ZFS_MODULE_PARAM(zfs_vnops, zfs_vnops_, read_chunk_size, ULONG, ZMOD_RW,
	"Bytes to read per chunk");
