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
# Copyright 2017, loli10K. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/cli_root/zpool_add/zpool_add.shlib

#
# DESCRIPTION:
#	'zpool add -o ashift=<n> ...' should work with different ashift
#	values.
#
# STRATEGY:
#	1. Create a pool with default values.
#	2. Verify 'zpool add -o ashift=<n>' works with allowed values (9-16).
#	3. Verify 'zpool add -o ashift=<n>' doesn't accept other invalid values.
#

verify_runnable "global"

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	rm -f $filedisk0 $filedisk1
}

log_assert "zpool add -o ashift=<n>' works with different ashift values"
log_onexit cleanup

disk1=$TEST_BASE_DIR/filedisk0
disk2=$TEST_BASE_DIR/filedisk1
log_must mkfile $SIZE $filedisk0
log_must mkfile $SIZE $filedisk1

typeset ashifts=("9" "10" "11" "12" "13" "14" "15" "16")
for ashift in ${ashifts[@]}
do
	log_must zpool create $TESTPOOL $filedisk0
	log_must zpool add -o ashift=$ashift $TESTPOOL $filedisk1
	verify_ashift $filedisk1 $ashift
	if [[ $? -ne 0 ]]
	then
		log_fail "Device was added without setting ashift value to "\
		    "$ashift"
	fi
	# clean things for the next run
	log_must zpool destroy $TESTPOOL
	log_must zpool labelclear $filedisk0
	log_must zpool labelclear $filedisk1
done

typeset badvals=("off" "on" "1" "8" "17" "1b" "ff" "-")
for badval in ${badvals[@]}
do
	log_must zpool create $TESTPOOL $filedisk0
	log_mustnot zpool add $TESTPOOL -o ashift="$badval" $filedisk1
	# clean things for the next run
	log_must zpool destroy $TESTPOOL
	log_must zpool labelclear $filedisk0
	log_mustnot zpool labelclear $filedisk1
done

log_pass "zpool add -o ashift=<n>' works with different ashift values"
