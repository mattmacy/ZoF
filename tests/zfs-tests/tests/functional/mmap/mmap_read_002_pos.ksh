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
# Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright (c) 2013, 2016 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/mmap/mmap.cfg

#
# DESCRIPTION:
# read()s from mmap()'ed file contain correct data.
#
# STRATEGY:
# 1. Create a pool & dataset
# 2. Call readmmap binary
# 3. unmount this file system
# 4. Verify the integrity of this pool & dateset
#

verify_runnable "global"

log_assert "read()s from mmap()'ed file contain correct data."

log_must eval "echo 123456123456123456 > $TESTDIR/test-read-file"
typeset checksum=$(sha256digest $TESTDIR/test-read-file)

log_must zfs snapshot $TESTPOOL/$TESTFS@original

log_must readmmap $TESTDIR/test-read-file

# Overwrite file with new contents
log_must eval "echo 234561234561234561 > $TESTDIR/test-read-file"
typeset checksum_new=$(sha256digest $TESTDIR/test-read-file)

log_must readmmap $TESTDIR/test-read-file

# Roll back file to old contents
log_must zfs rollback $TESTPOOL/$TESTFS@original

log_must readmmap $TESTDIR/test-read-file

typeset checksum_still=$(sha256digest $TESTDIR/test-read-file)
[[ "$checksum_still" == "$checksum" ]] || \
    log_fail "$checksum_still != $checksum (new: $checksum_new)"

verify_filesys "$TESTPOOL" "$TESTPOOL/$TESTFS" "$(get_device_dir $DISKS)"

log_pass "read(2) calls from a mmap(2)'ed file rolled back succeeded."
