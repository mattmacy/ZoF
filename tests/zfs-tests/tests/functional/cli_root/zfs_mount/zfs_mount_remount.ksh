#!/bin/ksh -p
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2017, loli10K <ezomori.nozomu@gmail.com>. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/cli_root/zfs_mount/zfs_mount.kshlib

#
# DESCRIPTION:
# Verify remount functionality, expecially on readonly objects.
#
# STRATEGY:
# 1. Prepare a filesystem and a snapshot
# 2. Verify we can (re)mount the dataset readonly/read-write
# 3. Verify we can mount the snapshot and it's mounted readonly
# 4. Verify we can't remount it read-write
# 5. Verify we can remount a dataset readonly and unmount it with
#    encryption=on and sync=disabled (issue #7753)
# 6. Re-import the pool readonly
# 7. Verify we can't remount its filesystem read-write
#

if is_freebsd; then
	log_unsupported "Remount not supported on FreeBSD"
fi

verify_runnable "both"

function cleanup
{
	log_must_busy zpool export $TESTPOOL
	log_must zpool import $TESTPOOL
	snapexists $TESTSNAP && log_must zfs destroy $TESTSNAP
	[[ -d $MNTPSNAP ]] && log_must rmdir $MNTPSNAP
	return 0
}

#
# Verify the $filesystem is mounted readonly
# This is preferred over "log_mustnot touch $fs" because we actually want to
# verify the error returned is EROFS
#
function readonlyfs # filesystem
{
	typeset filesystem="$1"

	file_write -o create -f $filesystem/file.dat
	ret=$?
	if [[ $ret != 30 ]]; then
		log_fail "Writing to $filesystem did not return EROFS ($ret)."
	fi
}

#
# Verify $dataset is mounted with $option
#
function checkmount # dataset option
{
	typeset dataset="$1"
	typeset option="$2"

	#Unable to check options on FreeBSD since procfs is not supported
	if [ is_freebsd ];then
		msg=$(mount | awk -v ds="$dataset" '$1 == ds { print $1 }')
		if [[ $msg == $dataset ]]; then
			log_note "Dataset $dataset is mounted"
		else
			log_fail "Dataset $dataset is not mounted"
		fi
	else
		options="$(awk -v ds="$dataset" '$1 == ds { print $4 }' /proc/mounts)"
		if [[ "$options" == '' ]]; then
			log_fail "Dataset $dataset is not mounted"
		elif [[ ! -z "${options##*$option*}" ]]; then
			log_fail "Dataset $dataset is not mounted with expected "\
			    "option $option ($options)"
		else
			log_note "Dataset $dataset is mounted with option $option"
		fi
	fi
}

log_assert "Verify remount functionality on both filesystem and snapshots"

log_onexit cleanup

# 1. Prepare a filesystem and a snapshot
TESTFS=$TESTPOOL/$TESTFS
TESTSNAP="$TESTFS@snap"
datasetexists $TESTFS || log_must zfs create $TESTFS
snapexists $TESTSNAP || log_must zfs snapshot $TESTSNAP
log_must zfs set readonly=off $TESTFS
MNTPFS="$(get_prop mountpoint $TESTFS)"
MNTPSNAP="$TESTDIR/zfs_snap_mount"
log_must mkdir -p $MNTPSNAP

# 2. Verify we can (re)mount the dataset readonly/read-write
log_must touch $MNTPFS/file.dat
checkmount $TESTFS 'rw'
if [ is_freebsd ];then
	#Remount not a supported option on FreeBSD
	log_must mount -o ro $TESTFS $MNTPFS
else
	log_must mount -o remount,ro $TESTFS $MNTPFS
fi
readonlyfs $MNTPFS
checkmount $TESTFS 'ro'
if [ is_freebsd ];then
	log_must mount -o rw $TESTFS $MNTPFS
else
	log_must mount -o remount,rw $TESTFS $MNTPFS
fi
log_must touch $MNTPFS/file.dat
checkmount $TESTFS 'rw'

# 3. Verify we can (re)mount the snapshot readonly
log_must mount -t zfs $TESTSNAP $MNTPSNAP
readonlyfs $MNTPSNAP
checkmount $TESTSNAP 'ro'
if [ is_freebsd ];then
	log_must mount -o ro $TESTSNAP $MNTPSNAP
else
	log_must mount -o remount,ro $TESTSNAP $MNTPSNAP
fi
readonlyfs $MNTPSNAP
checkmount $TESTSNAP 'ro'
log_must umount $MNTPSNAP

# 4. Verify we can't remount a snapshot read-write
# The "mount -o rw" command will succeed but the snapshot is mounted readonly.
# The "mount -o remount,rw" command must fail with an explicit error.
log_must mount -t zfs -o rw $TESTSNAP $MNTPSNAP
readonlyfs $MNTPSNAP
checkmount $TESTSNAP 'ro'
if [ is_freebsd ];then
	log_mustnot mount -o rw $TESTSNAP $MNTPSNAP
else
	log_mustnot mount -o remount,rw $TESTSNAP $MNTPSNAP
fi
readonlyfs $MNTPSNAP
checkmount $TESTSNAP 'ro'
log_must umount $MNTPSNAP

# 5. Verify we can remount a dataset readonly and unmount it with
#    encryption=on and sync=disabled (issue #7753)
log_must eval "echo 'password' | zfs create -o sync=disabled \
    -o encryption=on -o keyformat=passphrase $TESTFS/crypt"
CRYPT_MNTPFS="$(get_prop mountpoint $TESTFS/crypt)"
log_must touch $CRYPT_MNTPFS/file.dat
if [ is_freebsd ];then
	log_must mount -o ro $TESTFS/crypt $CRYPT_MNTPFS
else
	log_must mount -o remount,ro $TESTFS/crypt $CRYPT_MNTPFS
fi
log_must umount -f $CRYPT_MNTPFS
zpool sync $TESTPOOL

# 6. Re-import the pool readonly
log_must zpool export $TESTPOOL
log_must zpool import -o readonly=on $TESTPOOL

# 7. Verify we can't remount its filesystem read-write
readonlyfs $MNTPFS
checkmount $TESTFS 'ro'
if [ is_freebsd ];then
	log_mustnot mount -o rw $MNTPFS
else
	log_mustnot mount -o remount,rw $MNTPFS
fi
readonlyfs $MNTPFS
checkmount $TESTFS 'ro'

log_pass "Both filesystem and snapshots can be remounted correctly."
