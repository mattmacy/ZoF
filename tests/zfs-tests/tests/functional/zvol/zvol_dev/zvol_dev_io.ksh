#! /bin/ksh -p
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
# Copyright (c) 2018 by Lawrence Livermore National Security, LLC.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/io/io.cfg

#
# DESCRIPTION:
#	Verify basic pread(2) and pwrite(2).
#
# STRATEGY:
#	1. Use fio(1) in verify mode to perform write, read,
#	   random read, and random write workloads.
#	2. Repeat the test with additional fio(1) options.
#

verify_runnable "global"

function cleanup
{
	datasetexists $TESTPOOL/$TESTVOL && destroy_dataset $TESPTOOL/$TESTVOL
}

log_assert "Verify basic pread(2), pwrite(2) on volmode=dev zvol"

log_onexit cleanup

log_must zfs create -V $VOLSIZE -o volmode=dev $TESTPOOL/$TESTVOL

ioengine="--ioengine=psync"
dev="--filename=${ZVOL_DEVDIR}/$TESTPOOL/$TESTVOL"

set -A fio_arg -- "--sync=0" "--sync=1" "--direct=0" "--direct=1"

for arg in "${fio_arg[@]}"; do
	log_must fio $dev $ioengine $arg $FIO_WRITE_ARGS
	log_must fio $dev $ioengine $arg $FIO_READ_ARGS
	log_must fio $dev $ioengine $arg $FIO_RANDWRITE_ARGS
	log_must fio $dev $ioengine $arg $FIO_RANDREAD_ARGS
done

log_pass "Verified basic pread(2), pwrite(2) on volmode=dev zvol"
