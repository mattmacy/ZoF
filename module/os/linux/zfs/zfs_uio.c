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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*	Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T	*/
/*	  All Rights Reserved	*/

/*
 * University Copyright- Copyright (c) 1982, 1986, 1988
 * The Regents of the University of California
 * All Rights Reserved
 *
 * University Acknowledgment- Portions of this document are derived from
 * software developed by the University of California, Berkeley, and its
 * contributors.
 */
/*
 * Copyright (c) 2015 by Chunwei Chen. All rights reserved.
 */

#ifdef _KERNEL

#include <sys/types.h>
#include <sys/uio_impl.h>
#include <sys/zfs_debug.h>
#include <sys/sysmacros.h>
#include <sys/strings.h>
#include <linux/kmap_compat.h>
#include <linux/uaccess.h>
#include <sys/errno.h>
#include <sys/kmem.h>

/*
 * Move "n" bytes at byte address "p"; "rw" indicates the direction
 * of the move, and the I/O parameters are provided in "uio", which is
 * update to reflect the data which was moved.  Returns 0 on success or
 * a non-zero errno on failure.
 */
static int
zfs_uiomove_iov(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio)
{
	const struct iovec *iov = uio->uio_iov;
	size_t skip = uio->uio_skip;
	ulong_t cnt;

	while (n && uio->uio_resid) {
		cnt = MIN(iov->iov_len - skip, n);
		switch (uio->uio_segflg) {
		case UIO_USERSPACE:
			/*
			 * p = kernel data pointer
			 * iov->iov_base = user data pointer
			 */
			if (rw == UIO_READ) {
				if (copy_to_user(iov->iov_base+skip, p, cnt))
					return (EFAULT);
			} else {
				unsigned long b_left = 0;
				if (uio->uio_fault_disable) {
					if (!zfs_access_ok(VERIFY_READ,
					    (iov->iov_base + skip), cnt)) {
						return (EFAULT);
					}
					pagefault_disable();
					b_left =
					    __copy_from_user_inatomic(p,
					    (iov->iov_base + skip), cnt);
					pagefault_enable();
				} else {
					b_left =
					    copy_from_user(p,
					    (iov->iov_base + skip), cnt);
				}
				if (b_left > 0) {
					unsigned long c_bytes =
					    cnt - b_left;
					uio->uio_skip += c_bytes;
					ASSERT3U(uio->uio_skip, <,
					    iov->iov_len);
					uio->uio_resid -= c_bytes;
					uio->uio_loffset += c_bytes;
					return (EFAULT);
				}
			}
			break;
		case UIO_SYSSPACE:
			if (rw == UIO_READ)
				bcopy(p, iov->iov_base + skip, cnt);
			else
				bcopy(iov->iov_base + skip, p, cnt);
			break;
		default:
			ASSERT(0);
		}
		skip += cnt;
		if (skip == iov->iov_len) {
			skip = 0;
			uio->uio_iov = (++iov);
			uio->uio_iovcnt--;
		}
		uio->uio_skip = skip;
		uio->uio_resid -= cnt;
		uio->uio_loffset += cnt;
		p = (caddr_t)p + cnt;
		n -= cnt;
	}
	return (0);
}

static int
zfs_uiomove_bvec(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio)
{
	const struct bio_vec *bv = uio->uio_bvec;
	size_t skip = uio->uio_skip;
	ulong_t cnt;

	while (n && uio->uio_resid) {
		void *paddr;
		cnt = MIN(bv->bv_len - skip, n);

		paddr = zfs_kmap_atomic(bv->bv_page, KM_USER1);
		if (rw == UIO_READ)
			bcopy(p, paddr + bv->bv_offset + skip, cnt);
		else
			bcopy(paddr + bv->bv_offset + skip, p, cnt);
		zfs_kunmap_atomic(paddr, KM_USER1);

		skip += cnt;
		if (skip == bv->bv_len) {
			skip = 0;
			uio->uio_bvec = (++bv);
			uio->uio_iovcnt--;
		}
		uio->uio_skip = skip;
		uio->uio_resid -= cnt;
		uio->uio_loffset += cnt;
		p = (caddr_t)p + cnt;
		n -= cnt;
	}
	return (0);
}

#if defined(HAVE_VFS_IOV_ITER)
static int
zfs_uiomove_iter(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio,
    boolean_t revert)
{
	size_t cnt = MIN(n, uio->uio_resid);

	if (uio->uio_skip)
		iov_iter_advance(uio->uio_iter, uio->uio_skip);

	if (rw == UIO_READ)
		cnt = copy_to_iter(p, cnt, uio->uio_iter);
	else
		cnt = copy_from_iter(p, cnt, uio->uio_iter);

	/*
	 * When operating on a full pipe no bytes are processed.
	 * In which case return EFAULT which is converted to EAGAIN
	 * by the kernel's generic_file_splice_read() function.
	 */
	if (cnt == 0)
		return (EFAULT);

	/*
	 * Revert advancing the uio_iter.  This is set by zfs_uiocopy()
	 * to avoid consuming the uio and its iov_iter structure.
	 */
	if (revert)
		iov_iter_revert(uio->uio_iter, cnt);

	uio->uio_resid -= cnt;
	uio->uio_loffset += cnt;

	return (0);
}
#endif

int
zfs_uiomove(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio)
{
	if (uio->uio_segflg == UIO_BVEC)
		return (zfs_uiomove_bvec(p, n, rw, uio));
#if defined(HAVE_VFS_IOV_ITER)
	else if (uio->uio_segflg == UIO_ITER)
		return (zfs_uiomove_iter(p, n, rw, uio, B_FALSE));
#endif
	else
		return (zfs_uiomove_iov(p, n, rw, uio));
}
EXPORT_SYMBOL(zfs_uiomove);

/*
 * Fault in the pages of the first n bytes specified by the uio structure.
 * 1 byte in each page is touched and the uio struct is unmodified. Any
 * error will terminate the process as this is only a best attempt to get
 * the pages resident.
 */
int
zfs_uio_prefaultpages(ssize_t n, zfs_uio_t *uio)
{
	if (uio->uio_segflg == UIO_SYSSPACE || uio->uio_segflg == UIO_BVEC) {
		/* There's never a need to fault in kernel pages */
		return (0);
#if defined(HAVE_VFS_IOV_ITER)
	} else if (uio->uio_segflg == UIO_ITER) {
		/*
		 * At least a Linux 4.9 kernel, iov_iter_fault_in_readable()
		 * can be relied on to fault in user pages when referenced.
		 */
		if (iov_iter_fault_in_readable(uio->uio_iter, n))
			return (EFAULT);
#endif
	} else {
		/* Fault in all user pages */
		ASSERT3S(uio->uio_segflg, ==, UIO_USERSPACE);
		const struct iovec *iov = uio->uio_iov;
		int iovcnt = uio->uio_iovcnt;
		size_t skip = uio->uio_skip;
		uint8_t tmp;
		caddr_t p;

		for (; n > 0 && iovcnt > 0; iov++, iovcnt--, skip = 0) {
			ulong_t cnt = MIN(iov->iov_len - skip, n);
			/* empty iov */
			if (cnt == 0)
				continue;
			n -= cnt;
			/* touch each page in this segment. */
			p = iov->iov_base + skip;
			while (cnt) {
				if (get_user(tmp, (uint8_t *)p))
					return (EFAULT);
				ulong_t incr = MIN(cnt, PAGESIZE);
				p += incr;
				cnt -= incr;
			}
			/* touch the last byte in case it straddles a page. */
			p--;
			if (get_user(tmp, (uint8_t *)p))
				return (EFAULT);
		}
	}

	return (0);
}
EXPORT_SYMBOL(zfs_uio_prefaultpages);

/*
 * The same as zfs_uiomove() but doesn't modify uio structure.
 * return in cbytes how many bytes were copied.
 */
int
zfs_uiocopy(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio, size_t *cbytes)
{
	zfs_uio_t uio_copy;
	int ret;

	bcopy(uio, &uio_copy, sizeof (zfs_uio_t));

	if (uio->uio_segflg == UIO_BVEC)
		ret = zfs_uiomove_bvec(p, n, rw, &uio_copy);
#if defined(HAVE_VFS_IOV_ITER)
	else if (uio->uio_segflg == UIO_ITER)
		ret = zfs_uiomove_iter(p, n, rw, &uio_copy, B_TRUE);
#endif
	else
		ret = zfs_uiomove_iov(p, n, rw, &uio_copy);

	*cbytes = uio->uio_resid - uio_copy.uio_resid;

	return (ret);
}
EXPORT_SYMBOL(zfs_uiocopy);

/*
 * Drop the next n chars out of *uio.
 */
void
zfs_uioskip(zfs_uio_t *uio, size_t n)
{
	if (n > uio->uio_resid)
		return;

	if (uio->uio_segflg == UIO_BVEC) {
		uio->uio_skip += n;
		while (uio->uio_iovcnt &&
		    uio->uio_skip >= uio->uio_bvec->bv_len) {
			uio->uio_skip -= uio->uio_bvec->bv_len;
			uio->uio_bvec++;
			uio->uio_iovcnt--;
		}
#if defined(HAVE_VFS_IOV_ITER)
	} else if (uio->uio_segflg == UIO_ITER) {
		iov_iter_advance(uio->uio_iter, n);
#endif
	} else {
		uio->uio_skip += n;
		while (uio->uio_iovcnt &&
		    uio->uio_skip >= uio->uio_iov->iov_len) {
			uio->uio_skip -= uio->uio_iov->iov_len;
			uio->uio_iov++;
			uio->uio_iovcnt--;
		}
	}
	uio->uio_loffset += n;
	uio->uio_resid -= n;
}
EXPORT_SYMBOL(zfs_uioskip);

static void
zfs_uio_set_pages_to_stable(zfs_uio_t *uio)
{
	ASSERT3P(uio->uio_dio.pages, !=, NULL);
	for (int i = 0; i < uio->uio_dio.num_pages; i++) {
		struct page *p = uio->uio_dio.pages[i];
		ASSERT3P(p, !=, NULL);
		lock_page(p);
		set_page_dirty(p);

		if (PageWriteback(p)) {
			wait_on_page_bit(p, PG_writeback);
		}
		TestSetPageWriteback(p);
	}
}

static void
zfs_uio_release_stable_pages(zfs_uio_t *uio)
{
	ASSERT3P(uio->uio_dio.pages, !=, NULL);
	for (int i = 0; i < uio->uio_dio.num_pages; i++) {
		struct page *p = uio->uio_dio.pages[i];
		ASSERT3P(p, !=, NULL);
		ASSERT(PageLocked(p));
		end_page_writeback(p);
		unlock_page(p);
	}
}

void
zfs_uio_free_dio_pages(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	ASSERT3P(uio->uio_dio.pages, !=, NULL);
	if (rw == UIO_WRITE)
		zfs_uio_release_stable_pages(uio);
	for (int i = 0; i < uio->uio_dio.num_pages; i++) {
		struct page *p = uio->uio_dio.pages[i];
		if (p) {
			if (rw == UIO_READ)
				set_page_dirty(p);
			put_page(p);
		}
	}
#if defined HAVE_IOV_ITER_GET_PAGES_ALLOC
	kvfree(uio->uio_dio.pages);
#else
	kmem_free(uio->uio_dio.pages,
	    uio->uio_dio.num_pages * sizeof (struct page *));
#endif
}
EXPORT_SYMBOL(zfs_uio_free_dio_pages);

static int
zfs_uio_get_dio_pages_alloc_bvec(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	const struct bio_vec *bv = uio->uio_bvec;
	size_t skip = uio->uio_skip;
	ulong_t cnt;
	size_t maxsize = uio->uio_resid - skip;
	int n_pages = 0;

	uio->uio_dio.pages = kmem_alloc(sizeof (struct page *) *
	    DIV_ROUND_UP(maxsize, PAGE_SIZE), KM_SLEEP);
	if (!uio->uio_dio.pages)
		return (SET_ERROR(ENOMEM));
	uio->uio_dio.start = bv->bv_offset;

	while (maxsize) {
		if (!bv->bv_len) {
			continue;
			bv++;
		}
		cnt = MIN(bv->bv_len - skip, maxsize);
		get_page(uio->uio_dio.pages[n_pages] = bv->bv_page);
		skip += cnt;
		if (skip == bv->bv_len) {
			skip = 0;
			bv++;
		}
		maxsize -= cnt;
		n_pages++;
	}
	ASSERT0(maxsize);
	uio->uio_dio.num_pages = n_pages;
	return (0);
}

#if defined(HAVE_IOV_ITER_GET_PAGES_ALLOC)
static int
zfs_uio_get_dio_pages_alloc_iter(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	int numpages;
	ssize_t bytes;

	ASSERT(uio->uio_segflg == UIO_ITER);

	bytes = iov_iter_get_pages_alloc(uio->uio_iter,
	    &uio->uio_dio.pages, uio->uio_resid, &uio->uio_dio.start);
	if (bytes < 0)
		return (SET_ERROR(-bytes));
	numpages = DIV_ROUND_UP(uio->uio_dio.start + bytes, PAGE_SIZE);
	uio->uio_dio.num_pages = numpages;
	return (0);
}

#else
/*
 * Both zfs_uio_iov_step() and zfs_uio_get_dio_pages_alloc_iov() are merely
 * modified functions of the Linux kernel function
 * iov_iter_get_pages_alloc(). This function was not introduced till
 * kernel 3.16, so this code is used instead if it is not avaiable to
 * pin user pages from an uio_t iovec struct.
 */
static size_t
zfs_uio_iov_step(struct iovec v, zfs_uio_rw_t rw, zfs_uio_t *uio, int *numpages)
{
	struct page **p;
	unsigned long addr = (unsigned long)(v.iov_base);
	size_t len = v.iov_len +
	    (uio->uio_dio.start = addr & (PAGE_SIZE - 1));
	int n;
	int res;

	addr &= ~(PAGE_SIZE - 1);
	n = DIV_ROUND_UP(len, PAGE_SIZE);
	p = kmem_alloc(n * sizeof (struct page *), KM_SLEEP);
	if (!p) {
		*numpages = -1;
		return (SET_ERROR(ENOMEM));
	}
	res = zfs_get_user_pages(addr, n, rw == UIO_READ, p);
	if (res < 0) {
		kmem_free(p, n * sizeof (struct page *));
		*numpages = -1;
		return (SET_ERROR(-res));
	}
	uio->uio_dio.pages = p;
	*numpages += res;
	return ((res == n ? len : res * PAGE_SIZE) - uio->uio_dio.start);
}

static int
zfs_uio_get_dio_pages_alloc_iov(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	size_t left;
	struct iovec __v;
#if defined(HAVE_VFS_IOV_ITER)
	const struct iovec *__p = uio->uio_iter->iov;
#else
	const struct iovec *__p = uio->uio_iov;
#endif
	size_t skip = uio->uio_skip;
	size_t wanted;
	size_t maxsize;
	int numpages = 0;

	ASSERT(uio->uio_segflg == UIO_USERSPACE);
	wanted = maxsize = uio->uio_resid - skip;

	__v.iov_len = MIN(maxsize, __p->iov_len - skip);
	if (__v.iov_len) {
		__v.iov_base = __p->iov_base + skip;
		left = zfs_uio_iov_step(__v, rw, uio, &numpages);
		if (numpages == -1)
			return (left);
		__v.iov_len -= left;
		maxsize -= __v.iov_len;
	} else {
		left = 0;
	}

	while (!left && maxsize) {
		__p++;
		__v.iov_len = MIN(maxsize, __p->iov_len);
		if (!__v.iov_len)
			continue;
		__v.iov_base = __p->iov_base;
		left = zfs_uio_iov_step(__v, rw, uio, &numpages);
		if (numpages == -1)
			return (left);
		__v.iov_len -= left;
		maxsize -= __v.iov_len;
	}

	ASSERT0(wanted - maxsize);
	uio->uio_dio.num_pages = numpages;
	return (0);
}

#endif /* HAVE_IOV_ITER_GET_PAGES_ALLOC */

/*
 * This function allocates kernel pages and pins user pages into them.
 * In the event that the user pages were not pinned successfully an error
 * value is returned.
 *
 * On success, 0 is returned.
 */
int
zfs_uio_get_dio_pages_alloc(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	int error = 0;

	if (uio->uio_segflg == UIO_BVEC)
		error = zfs_uio_get_dio_pages_alloc_bvec(uio, rw);
#if defined(HAVE_IOV_ITER_GET_PAGES_ALLOC)
	else if (uio->uio_segflg == UIO_ITER)
		error = zfs_uio_get_dio_pages_alloc_iter(uio, rw);
#else
	else
		error = zfs_uio_get_dio_pages_alloc_iov(uio, rw);
#endif

	if (error)
		return (error);

	/*
	 * Since we will be writing the user pages we must make sure that
	 * they are stable. That way the contents of the pages can not
	 * change in the event we are doing any of the following:
	 * Compression
	 * Checksum
	 * Encryption
	 * Parity
	 * Dedup
	 */
	if (rw == UIO_WRITE)
		zfs_uio_set_pages_to_stable(uio);
	return (0);
}
EXPORT_SYMBOL(zfs_uio_get_dio_pages_alloc);

boolean_t
zfs_uio_page_aligned(zfs_uio_t *uio)
{
	if (IO_PAGE_ALIGNED(uio->uio_loffset, uio->uio_resid))
		return (B_TRUE);
	return (B_FALSE);
}
EXPORT_SYMBOL(zfs_uio_page_aligned);

#endif /* _KERNEL */
