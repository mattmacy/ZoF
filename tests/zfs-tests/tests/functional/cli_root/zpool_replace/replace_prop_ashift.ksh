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
. $STF_SUITE/tests/functional/cli_root/zpool_create/zpool_create.shlib

#
# DESCRIPTION:
#	'zpool replace' should use the ashift pool property value as default.
#
# STRATEGY:
#	1. Create a pool with default values.
#	2. Verify 'zpool replace' uses the ashift pool property value when
#	   replacing an existing device.
#	3. Verify the default ashift value can still be overridden by manually
#	   specifying '-o ashift=<n>' from the command line.
#

verify_runnable "global"

function cleanup
{
	poolexists $TESTPOOL1 && destroy_pool $TESTPOOL1
	rm -f $filedisk1 $filedisk2
}

log_assert "'zpool replace' uses the ashift pool property value as default."
log_onexit cleanup

filedisk0=$TEST_BASE_DIR/filedisk0
filedisk1=$TEST_BASE_DIR/filedisk1
log_must truncate -s $SIZE $filedisk0
log_must truncate -s $SIZE $filedisk1

typeset ashifts=("9" "10" "11" "12" "13" "14" "15" "16")
for ashift in ${ashifts[@]}
do
	for pprop in ${ashifts[@]}
	do
		log_must zpool create -o ashift=$ashift $TESTPOOL1 $filedisk0
		log_must zpool set ashift=$pprop $TESTPOOL1
		# ashift_of(replacing_disk) <= ashift_of(existing_vdev)
		if [[ $pprop -le $ashift ]]
		then
			log_must zpool replace $TESTPOOL1 $filedisk0 $filedisk1
			wait_replacing $TESTPOOL1
			verify_ashift $filedisk1 $ashift
			if [[ $? -ne 0 ]]
			then
				log_fail "Device was replaced without " \
				    "setting ashift value to $ashift"
			fi
		else
			# cannot replace if pool prop ashift > vdev ashift
			log_mustnot zpool replace $TESTPOOL1 \
			    $filedisk0 $filedisk1
			# verify we can override the pool prop value manually
			log_must zpool replace -o ashift=$ashift $TESTPOOL1 \
			    $filedisk0 $filedisk1
			wait_replacing $TESTPOOL1
			verify_ashift $filedisk1 $ashift
			if [[ $? -ne 0 ]]
			then
				log_fail "Device was replaced without " \
				    "setting ashift value to $ashift"
			fi
		fi
		# clean things for the next run
		log_must zpool destroy $TESTPOOL1
		log_must zpool labelclear $filedisk0
		log_must zpool labelclear $filedisk1
	done
done

log_pass "'zpool replace' uses the ashift pool property value."
