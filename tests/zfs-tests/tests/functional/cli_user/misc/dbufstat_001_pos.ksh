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
# Copyright (c) 2015 by Lawrence Livermore National Security, LLC.
# All rights reserved.
#

. $STF_SUITE/include/libtest.shlib

if is_freebsd; then
	log_unsupported "dbufstat.py relies on procfs, which is not supported on FreeBSD"
fi

set -A args  "" "-b" "-d" "-r" "-v" "-s \",\"" "-x" "-n"

log_assert "dbufstat.py generates output and doesn't return an error code"

typeset -i i=0
while [[ $i -lt ${#args[*]} ]]; do
	if is_freebsd; then
		log_must eval "python /usr/local/bin/dbufstat.py ${args[i]} > /dev/null"
	else
		log_must eval "dbufstat.py ${args[i]} > /dev/null"
	fi
        ((i = i + 1))
done

# A simple test of dbufstat.py filter functionality
if is_freebsd; then
	log_must eval "python /usr/local/bin/dbufstat.py -F object=10,dbc=1,pool=$TESTPOOL > /dev/null"
else
	log_must eval "dbufstat.py -F object=10,dbc=1,pool=$TESTPOOL > /dev/null"
fi

log_pass "dbufstat.py generates output and doesn't return an error code"
