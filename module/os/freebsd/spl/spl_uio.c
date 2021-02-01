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

/* Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T */
/* All Rights Reserved   */

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
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/zfs_znode.h>
#include <sys/byteorder.h>
#include <sys/lock.h>
#include <sys/vm.h>
#include <vm/vm_map.h>

/*
 * same as zfs_uiomove() but doesn't modify uio structure.
 * return in cbytes how many bytes were copied.
 */
int
zfs_uiocopy(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio, size_t *cbytes)
{
	struct iovec small_iovec[1];
	struct uio small_uio_clone;
	struct uio *uio_clone;
	int error;

	ASSERT3U(zfs_uio_rw(uio), ==, rw);
	if (zfs_uio_iovcnt(uio) == 1) {
		small_uio_clone = *(GET_UIO_STRUCT(uio));
		small_iovec[0] = *(GET_UIO_STRUCT(uio)->uio_iov);
		small_uio_clone.uio_iov = small_iovec;
		uio_clone = &small_uio_clone;
	} else {
		uio_clone = cloneuio(GET_UIO_STRUCT(uio));
	}

	error = vn_io_fault_uiomove(p, n, uio_clone);
	*cbytes = zfs_uio_resid(uio) - uio_clone->uio_resid;
	if (uio_clone != &small_uio_clone)
		free(uio_clone, M_IOV);
	return (error);
}

/*
 * Drop the next n chars out of *uiop.
 */
void
zfs_uioskip(zfs_uio_t *uio, size_t n)
{
	zfs_uio_seg_t segflg;

	/* For the full compatibility with illumos. */
	if (n > zfs_uio_resid(uio))
		return;

	segflg = zfs_uio_segflg(uio);
	zfs_uio_segflg(uio) = UIO_NOCOPY;
	zfs_uiomove(NULL, n, zfs_uio_rw(uio), uio);
	zfs_uio_segflg(uio) = segflg;
}

int
zfs_uio_fault_move(void *p, size_t n, zfs_uio_rw_t dir, zfs_uio_t *uio)
{
	ASSERT(zfs_uio_rw(uio) == dir);
	return (vn_io_fault_uiomove(p, n, GET_UIO_STRUCT(uio)));
}


#if __FreeBSD_version < 1300050
static void
zfs_uio_set_pages_to_stable(zfs_uio_t *uio)
{
	vm_object_t obj;

	ASSERT3P(uio->uio_dio.pages, !=, NULL);
	ASSERT3U(uio->uio_dio.num_pages, >, 0);

	obj = uio->uio_dio.pages[0]->object;
	zfs_vmobject_wlock(obj);
	for (int i = 0; i < uio->uio_dio.num_pages; i++) {
		vm_page_t page = uio->uio_dio.pages[i];

		ASSERT3P(page, !=, NULL);
		MPASS(page == PHYS_TO_VM_PAGE(VM_PAGE_TO_PHYS(page)));
		vm_page_sbusy(page);
		if (page->object != obj) {
			zfs_vmobject_wunlock(obj);
			obj = page->object;
			zfs_vmobject_wlock(obj);
		}
		pmap_remove_write(page);
	}
	zfs_vmobject_wunlock(obj);
}

static void
zfs_uio_release_stable_pages(zfs_uio_t *uio)
{

	ASSERT3P(uio->uio_dio.pages, !=, NULL);
	for (int i = 0; i < uio->uio_dio.num_pages; i++) {
		vm_page_t page = uio->uio_dio.pages[i];

		ASSERT3P(page, !=, NULL);
		ASSERT(vm_page_sbusied(page));
		vm_page_sunbusy(page);
	}
	if (mtx != NULL)
		mtx_unlock(mtx);
}

#else

static void
zfs_uio_set_pages_to_stable(zfs_uio_t *uio)
{

	ASSERT3P(uio->uio_dio.pages, !=, NULL);
	ASSERT3U(uio->uio_dio.num_pages, >, 0);

	ASSERT3P(uio->uio_dio.pages, !=, NULL);
	for (int i = 0; i < uio->uio_dio.num_pages; i++) {
		vm_page_t page = uio->uio_dio.pages[i];

		MPASS(page == PHYS_TO_VM_PAGE(VM_PAGE_TO_PHYS(page)));
		vm_page_busy_acquire(page, VM_ALLOC_SBUSY);
		pmap_remove_write(page);
	}
}

static void
zfs_uio_release_stable_pages(zfs_uio_t *uio)
{

	ASSERT3P(uio->uio_dio.pages, !=, NULL);
	for (int i = 0; i < uio->uio_dio.num_pages; i++) {
		vm_page_t page = uio->uio_dio.pages[i];

		vm_page_sunbusy(page);
	}
}

#endif

/*
 * If the operation is marked as read, then we are stating the pages will be
 * written to and must be given write access.
 */
static int
zfs_uio_hold_pages(unsigned long start, unsigned long nr_pages, zfs_uio_rw_t rw,
    vm_page_t *pages)
{
	vm_map_t map;
	vm_prot_t prot;
	size_t len;
	int count;

	map = &curthread->td_proc->p_vmspace->vm_map;

	prot = rw == UIO_READ ? (VM_PROT_READ | VM_PROT_WRITE) : VM_PROT_READ;
	len = ((size_t)nr_pages) << PAGE_SHIFT;
	count = vm_fault_quick_hold_pages(map, start, len, prot, pages,
	    nr_pages);

	return (count);
}

static void
zfs_uio_unhold_pages(vm_page_t *m, int count)
{
	vm_page_unhold_pages(m, count);
}

void
zfs_uio_free_dio_pages(zfs_uio_t *uio, zfs_uio_rw_t rw)
{

	ASSERT3P(uio->uio_dio.pages, !=, NULL);
	ASSERT(zfs_uio_rw(uio) == rw);

	if (rw == UIO_WRITE)
		zfs_uio_release_stable_pages(uio);

#if __FreeBSD_version < 1300050
	for (int i = 0; i < uio->uio_dio.num_pages; i++) {
		vm_page_t page = uio->uio_dio.pages[i];
		ASSERT3P(page, !=, NULL);
		vm_page_lock(page);
		vm_page_unwire_noq(page);
		vm_page_unlock(page);
	}
#else
	zfs_uio_unhold_pages(&uio->uio_dio.pages[0],
	    uio->uio_dio.num_pages);

#endif

	kmem_free(uio->uio_dio.pages,
	    uio->uio_dio.num_pages * sizeof (vm_page_t));
}

static long
zfs_uio_get_user_pages(unsigned long start, unsigned long nr_pages,
    zfs_uio_rw_t rw, vm_page_t *pages)
{
	int count;

	count = zfs_uio_hold_pages(start, nr_pages, rw, pages);

	if (count != nr_pages) {
		if (count > 0)
		    zfs_uio_unhold_pages(pages, count);

		return (count);
	}
	ASSERT3U(count, ==, nr_pages);

#if __FreeBSD_version < 1300050
	for (int i = 0; i < nr_pages; i++) {
		vm_page_t page = pages[i];
		vm_page_lock(page);
		vm_page_wire(page);
		vm_page_unhold(page);
		vm_page_unlock(page);
	}
#endif

	return (count);
}

static size_t
zfs_uio_iov_step(struct iovec v, zfs_uio_t *uio, int *numpages)
{
	vm_page_t *pages;
	unsigned long addr = (unsigned long)(v.iov_base);
	size_t len = v.iov_len +
	    (uio->uio_dio.start = addr & (PAGE_SIZE - 1));
	int n;
	int res;

	addr &= ~(PAGE_SIZE - 1);
	n = DIV_ROUND_UP(len, PAGE_SIZE);
	pages = kmem_alloc(n * sizeof (vm_page_t), KM_SLEEP);
	if (!pages) {
		*numpages = -1;
		return (SET_ERROR(ENOMEM));
	}
	res = zfs_uio_get_user_pages(addr, n, zfs_uio_rw(uio), pages);
	if (res != n) {
		kmem_free(pages, n * sizeof (vm_page_t));
		*numpages = -1;
		return (SET_ERROR(EFAULT));
	}
	uio->uio_dio.pages = pages;
	*numpages += res;
	return ((res == n ? len : res * PAGE_SIZE) - uio->uio_dio.start);
}

static int
zfs_uio_get_dio_pages_alloc_impl(zfs_uio_t *uio)
{
	size_t left = 0;
	struct iovec __v;
	size_t wanted;
	const struct iovec *__p = GET_UIO_STRUCT(uio)->uio_iov;
	size_t maxsize = zfs_uio_resid(uio);
	int numpages = 0;

	wanted = maxsize;

	while (!left && maxsize) {
		__v.iov_len = MIN(maxsize, __p->iov_len);
		if (!__v.iov_len) {
			__p++;
			continue;
		}
		__v.iov_base = __p->iov_base;
		left = zfs_uio_iov_step(__v, uio, &numpages);
		if (numpages == -1)
			return (left);
		__v.iov_len -= left;
		maxsize -= __v.iov_len;
		__p++;
	}

	ASSERT0(wanted - maxsize);
	uio->uio_dio.num_pages = numpages;
	return (0);
}

/*
 * This function allocates kernel pages and pins user pages into them.
 * In the event that the user pages were not pinned successfully an error
 * value is reutrned.
 *
 * On success, 0 is returned.
 */
int
zfs_uio_get_dio_pages_alloc(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	int error = 0;

	ASSERT(zfs_uio_rw(uio) == rw);
	error = zfs_uio_get_dio_pages_alloc_impl(uio);

	if (error)
		return (error);

	/*
	 * Since we will be writing the user pages we must make sure that they
	 * are stable. That way the contents of the pages can not cahnge in the
	 * event we are doing any of the following:
	 * Compression
	 * Checksum
	 * Encryption
	 * Parity
	 * Dedup
	 */
	if (zfs_uio_rw(uio) == UIO_WRITE)
		zfs_uio_set_pages_to_stable(uio);
	return (0);
}

boolean_t
zfs_uio_page_aligned(zfs_uio_t *uio)
{
	if (IO_PAGE_ALIGNED(zfs_uio_offset(uio), zfs_uio_resid(uio)))
		return (B_TRUE);
	return (B_FALSE);
}
