/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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
 * Copyright 2020 iXsystems, Inc.
 */

#ifndef _LIBZFS_FREEBSD_H
#define	_LIBZFS_FREEBSD_H

#include <libzfs.h>

/*
 * Attach/detach the given filesystem to/from the given jail.
 */
extern int zfs_jail(zfs_handle_t *zhp, int jailid, int attach);

/*
 * Set loader options for next boot.
 */
extern int zpool_nextboot(libzfs_handle_t *, uint64_t, uint64_t, const char *);

#endif /* _LIBZFS_FREEBSD_H */
