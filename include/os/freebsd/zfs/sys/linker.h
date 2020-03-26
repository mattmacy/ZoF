/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 1997-2000 Doug Rabson
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
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

#ifndef _SYS_LINKER_H_
#define _SYS_LINKER_H_

/*
 * Module information subtypes
 */
#define MODINFO_END		0x0000		/* End of list */
#define MODINFO_NAME		0x0001		/* Name of module (string) */
#define MODINFO_TYPE		0x0002		/* Type of module (string) */
#define MODINFO_ADDR		0x0003		/* Loaded address */
#define MODINFO_SIZE		0x0004		/* Size of module */
#define MODINFO_EMPTY		0x0005		/* Has been deleted */
#define MODINFO_ARGS		0x0006		/* Parameters string */
#define MODINFO_METADATA	0x8000		/* Module-specfic */

#define MODINFOMD_AOUTEXEC	0x0001		/* a.out exec header */
#define MODINFOMD_ELFHDR	0x0002		/* ELF header */
#define MODINFOMD_SSYM		0x0003		/* start of symbols */
#define MODINFOMD_ESYM		0x0004		/* end of symbols */
#define MODINFOMD_DYNAMIC	0x0005		/* _DYNAMIC pointer */
/* These values are MD on these two platforms */
#if !defined(__sparc64__) && !defined(__powerpc__)
#define MODINFOMD_ENVP		0x0006		/* envp[] */
#define MODINFOMD_HOWTO		0x0007		/* boothowto */
#define MODINFOMD_KERNEND	0x0008		/* kernend */
#endif
#define MODINFOMD_SHDR		0x0009		/* section header table */
#define MODINFOMD_CTORS_ADDR	0x000a		/* address of .ctors */
#define MODINFOMD_CTORS_SIZE	0x000b		/* size of .ctors */
#define MODINFOMD_FW_HANDLE	0x000c		/* Firmware dependent handle */
#define MODINFOMD_KEYBUF	0x000d		/* Crypto key intake buffer */
#define MODINFOMD_NOCOPY	0x8000		/* don't copy this metadata to the kernel */

#define MODINFOMD_DEPLIST	(0x4001 | MODINFOMD_NOCOPY)	/* depends on */

#ifdef _KERNEL

/*
 * Module lookup
 */
extern vm_offset_t	preload_addr_relocate;
extern caddr_t		preload_metadata;

extern void *		preload_fetch_addr(caddr_t _mod);
extern size_t		preload_fetch_size(caddr_t _mod);
extern caddr_t		preload_search_by_name(const char *_name);
extern caddr_t		preload_search_by_type(const char *_type);
extern caddr_t		preload_search_next_name(caddr_t _base);
extern caddr_t		preload_search_info(caddr_t _mod, int _inf);
extern void		preload_delete_name(const char *_name);
extern void		preload_bootstrap_relocate(vm_offset_t _offset);

#endif /* _KERNEL */

#endif /* !_SYS_LINKER_H_ */
