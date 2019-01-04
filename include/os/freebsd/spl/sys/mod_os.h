/*
 * Copyright (c) 2020 iX Systems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#ifndef _SPL_MOD_H
#define	_SPL_MOD_H

#define	ZFS_MODULE_DESCRIPTION(s)
#define	ZFS_MODULE_AUTHOR(s)
#define	ZFS_MODULE_LICENSE(s)
#define	ZFS_MODULE_VERSION(s)

/* BEGIN CSTYLED */
#define	ZFS_MODULE_PARAM_CALL(scope_prefix, name_prefix, name, setfunc, getfunc, perm, desc)

#include <sys/kernel.h>
#define	module_init(fn)							\
static void \
wrap_ ## fn(void *dummy __unused) \
{								 \
	fn();						 \
}																		\
SYSINIT(zfs_ ## fn, SI_SUB_LAST, SI_ORDER_FIRST, wrap_ ## fn, NULL)


#define	module_exit(fn) 							\
static void \
wrap_ ## fn(void *dummy __unused) \
{								 \
	fn();						 \
}																		\
SYSUNINIT(zfs_ ## fn, SI_SUB_LAST, SI_ORDER_FIRST, wrap_ ## fn, NULL)
/* END CSTYLED */

#endif /* SPL_MOD_H */
