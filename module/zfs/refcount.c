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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2015 by Delphix. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/refcount.h>

/*
 * Reference count tracking is disabled by default.  It's memory requirements
 * are reasonable, however as implemented it consumes a significant amount of
 * cpu time.  Until its performance is improved it should be manually enabled.
 */
int reference_tracking_enable = FALSE;
int reference_history = 30; /* tunable */

#ifdef	ZFS_DEBUG
typedef struct reference {
	list_node_t ref_link;
	const void *ref_holder;
	uint64_t ref_number;
	uint8_t *ref_removed;
	const char *ref_add_file;
	const char *ref_rem_file;
	int ref_add_line;
	int ref_rem_line;
} reference_t;

static kmem_cache_t *reference_cache;
static kmem_cache_t *reference_history_cache;

void
zfs_refcount_init(void)
{
	reference_cache = kmem_cache_create("reference_cache",
	    sizeof (reference_t), 0, NULL, NULL, NULL, NULL, NULL, 0);

	reference_history_cache = kmem_cache_create("reference_history_cache",
	    sizeof (uint64_t), 0, NULL, NULL, NULL, NULL, NULL, 0);
}

void
zfs_refcount_fini(void)
{
	kmem_cache_destroy(reference_cache);
	kmem_cache_destroy(reference_history_cache);
}

void
zfs_refcount_create(zfs_refcount_t *rc)
{
	mutex_init(&rc->rc_mtx, NULL, MUTEX_DEFAULT, NULL);
	list_create(&rc->rc_list, sizeof (reference_t),
	    offsetof(reference_t, ref_link));
	list_create(&rc->rc_removed, sizeof (reference_t),
	    offsetof(reference_t, ref_link));
	rc->rc_count = 0;
	rc->rc_removed_count = 0;
	rc->rc_tracked = reference_tracking_enable;
}

void
zfs_refcount_create_tracked(zfs_refcount_t *rc)
{
	zfs_refcount_create(rc);
	rc->rc_tracked = B_TRUE;
}

void
zfs_refcount_create_untracked(zfs_refcount_t *rc)
{
	zfs_refcount_create(rc);
	rc->rc_tracked = B_FALSE;
}

void
zfs_refcount_destroy_many(zfs_refcount_t *rc, uint64_t number)
{
	reference_t *ref;

	while ((ref = list_head(&rc->rc_list))) {
		list_remove(&rc->rc_list, ref);
#if !defined(_KERNEL) || defined(__FreeBSD__)
		if (rc->rc_count != number)
			printf("ref_holder: %p ref_number: %lu %s:%d\n",
				   ref->ref_holder, (uint64_t)ref->ref_number, ref->ref_add_file, ref->ref_add_line);
#endif
		kmem_cache_free(reference_cache, ref);
	}
	list_destroy(&rc->rc_list);

	while ((ref = list_head(&rc->rc_removed))) {
		list_remove(&rc->rc_removed, ref);
#if !defined(_KERNEL) || defined(__FreeBSD__)
		if (rc->rc_count != number)
			printf("ref_holder: %p ref_number: %lu add %s:%d remove %s:%d\n",
				   ref->ref_holder, (uint64_t)ref->ref_number, ref->ref_add_file, ref->ref_add_line, ref->ref_rem_file, ref->ref_rem_line);
#endif
		kmem_cache_free(reference_history_cache, ref->ref_removed);
		kmem_cache_free(reference_cache, ref);
	}
	list_destroy(&rc->rc_removed);
	ASSERT3U(rc->rc_count, ==, number);

	mutex_destroy(&rc->rc_mtx);
}

void
zfs_refcount_destroy(zfs_refcount_t *rc)
{
	zfs_refcount_destroy_many(rc, 0);
}

int
zfs_refcount_is_zero(zfs_refcount_t *rc)
{
	return (rc->rc_count == 0);
}

int64_t
zfs_refcount_count(zfs_refcount_t *rc)
{
	return (rc->rc_count);
}

int64_t
zfs_refcount_add_many_(zfs_refcount_t *rc, uint64_t number, const void *holder, const char *file, int line)
{
	reference_t *ref = NULL;
	int64_t count;

	if (rc->rc_tracked) {
		ref = kmem_cache_alloc(reference_cache, KM_SLEEP);
		ref->ref_holder = holder;
		ref->ref_number = number;
		ref->ref_add_file = file;
		ref->ref_add_line = line;
	}
	mutex_enter(&rc->rc_mtx);
	ASSERT3U(rc->rc_count, >=, 0);
	if (rc->rc_tracked)
		list_insert_head(&rc->rc_list, ref);
	rc->rc_count += number;
	count = rc->rc_count;
	mutex_exit(&rc->rc_mtx);

	return (count);
}

int64_t
zfs_refcount_add_(zfs_refcount_t *rc, const void *holder, const char *file, int line)
{
	return (zfs_refcount_add_many_(rc, 1, holder, file, line));
}

int64_t
zfs_refcount_remove_many_(zfs_refcount_t *rc, uint64_t number,
    const void *holder, const char *file, int line)
{
	reference_t *ref;
	int64_t count;

	mutex_enter(&rc->rc_mtx);

	if (!rc->rc_tracked) {
		ASSERT3U(rc->rc_count, >=, number);
		rc->rc_count -= number;
		count = rc->rc_count;
		mutex_exit(&rc->rc_mtx);
		return (count);
	}
#if !defined(_KERNEL) || defined(__FreeBSD__)
	if (rc->rc_count < number) {
		printf("refs added:\n");
		for (ref = list_head(&rc->rc_list); ref;
			 ref = list_next(&rc->rc_list, ref)) {
			printf("ref_holder: %p ref_number: %lu %s:%d\n",
				   ref->ref_holder, (uint64_t)ref->ref_number, ref->ref_add_file, ref->ref_add_line);
		}
		printf("refs removed:\n");
		for (ref = list_head(&rc->rc_removed); ref;
			 ref = list_next(&rc->rc_removed, ref)) {
			printf("ref_holder: %p ref_number: %lu add %s:%d remove %s:%d\n",
				   ref->ref_holder, (uint64_t)ref->ref_number, ref->ref_add_file, ref->ref_add_line, ref->ref_rem_file, ref->ref_rem_line);

		}
		delay(25*hz);
	}
#endif
	ASSERT3U(rc->rc_count, >=, number);
	for (ref = list_head(&rc->rc_list); ref;
	    ref = list_next(&rc->rc_list, ref)) {
		if (ref->ref_holder == holder && ref->ref_number == number) {
			list_remove(&rc->rc_list, ref);
			ref->ref_rem_file = file;
			ref->ref_rem_line = line;
			if (reference_history > 0) {
				ref->ref_removed =
				    kmem_cache_alloc(reference_history_cache,
				    KM_SLEEP);
				list_insert_head(&rc->rc_removed, ref);
				rc->rc_removed_count++;
				if (rc->rc_removed_count > reference_history) {
					ref = list_tail(&rc->rc_removed);
					list_remove(&rc->rc_removed, ref);
					kmem_cache_free(reference_history_cache,
					    ref->ref_removed);
					kmem_cache_free(reference_cache, ref);
					rc->rc_removed_count--;
				}
			} else {
				kmem_cache_free(reference_cache, ref);
			}
			rc->rc_count -= number;
			count = rc->rc_count;
			mutex_exit(&rc->rc_mtx);
			return (count);
		}
	}
#if !defined(_KERNEL) || defined(__FreeBSD__)
	for (ref = list_head(&rc->rc_list); ref;
		 ref = list_next(&rc->rc_list, ref)) {
		printf("%p : %lu %s:%d\n", ref->ref_holder, ref->ref_number, ref->ref_add_file, ref->ref_add_line);
	}
#endif
	panic("No such hold %p on refcount %llx", holder,
	    (u_longlong_t)(uintptr_t)rc);
	return (-1);
}

int64_t
zfs_refcount_remove_(zfs_refcount_t *rc, const void *holder, const char *file, int line)
{
	return (zfs_refcount_remove_many_(rc, 1, holder, file, line));
}

void
zfs_refcount_transfer(zfs_refcount_t *dst, zfs_refcount_t *src)
{
	int64_t count, removed_count;
	list_t list, removed;

	list_create(&list, sizeof (reference_t),
	    offsetof(reference_t, ref_link));
	list_create(&removed, sizeof (reference_t),
	    offsetof(reference_t, ref_link));

	mutex_enter(&src->rc_mtx);
	count = src->rc_count;
	removed_count = src->rc_removed_count;
	src->rc_count = 0;
	src->rc_removed_count = 0;
	list_move_tail(&list, &src->rc_list);
	list_move_tail(&removed, &src->rc_removed);
	mutex_exit(&src->rc_mtx);

	mutex_enter(&dst->rc_mtx);
	dst->rc_count += count;
	dst->rc_removed_count += removed_count;
	list_move_tail(&dst->rc_list, &list);
	list_move_tail(&dst->rc_removed, &removed);
	mutex_exit(&dst->rc_mtx);

	list_destroy(&list);
	list_destroy(&removed);
}

void
zfs_refcount_transfer_ownership_many(zfs_refcount_t *rc, uint64_t number,
    const void *current_holder, const void *new_holder)
{
	reference_t *ref;
	boolean_t found = B_FALSE;

	mutex_enter(&rc->rc_mtx);
	if (!rc->rc_tracked) {
		mutex_exit(&rc->rc_mtx);
		return;
	}

	for (ref = list_head(&rc->rc_list); ref;
	    ref = list_next(&rc->rc_list, ref)) {
		if (ref->ref_holder == current_holder &&
		    ref->ref_number == number) {
			ref->ref_holder = new_holder;
			found = B_TRUE;
			break;
		}
	}
#if !defined(_KERNEL) || defined(__FreeBSD__)

	if (!found) {
		for (ref = list_head(&rc->rc_list); ref;
			 ref = list_next(&rc->rc_list, ref)) {
			if (ref->ref_holder == current_holder)
				printf("%p : %lu\n", current_holder, ref->ref_number);
		}
		for (ref = list_head(&rc->rc_list); ref;
			 ref = list_next(&rc->rc_list, ref)) {
			if (ref->ref_holder == new_holder) {
				panic("new_holder %p already in reference list with number %lu and old holder %p not found\n", new_holder, ref->ref_number, current_holder);
				found = B_TRUE;
				break;
			}
		}
		for (ref = list_head(&rc->rc_list); ref;
			 ref = list_next(&rc->rc_list, ref)) {
			printf("%p : %lu %s:%d\n", ref->ref_holder, ref->ref_number, ref->ref_add_file, ref->ref_add_line);
		}
	}
#endif
	ASSERT(found);
	mutex_exit(&rc->rc_mtx);
}

void
zfs_refcount_transfer_ownership(zfs_refcount_t *rc, const void *current_holder,
    const void *new_holder)
{
	return (zfs_refcount_transfer_ownership_many(rc, 1, current_holder,
	    new_holder));
}

/*
 * If tracking is enabled, return true if a reference exists that matches
 * the "holder" tag. If tracking is disabled, then return true if a reference
 * might be held.
 */
boolean_t
zfs_refcount_held(zfs_refcount_t *rc, const void *holder)
{
	reference_t *ref;

	mutex_enter(&rc->rc_mtx);

	if (!rc->rc_tracked) {
		mutex_exit(&rc->rc_mtx);
		return (rc->rc_count > 0);
	}

	for (ref = list_head(&rc->rc_list); ref;
	    ref = list_next(&rc->rc_list, ref)) {
		if (ref->ref_holder == holder) {
			mutex_exit(&rc->rc_mtx);
			return (B_TRUE);
		}
	}
	mutex_exit(&rc->rc_mtx);
	return (B_FALSE);
}

/*
 * If tracking is enabled, return true if a reference does not exist that
 * matches the "holder" tag. If tracking is disabled, always return true
 * since the reference might not be held.
 */
boolean_t
zfs_refcount_not_held(zfs_refcount_t *rc, const void *holder)
{
	reference_t *ref;

	mutex_enter(&rc->rc_mtx);

	if (!rc->rc_tracked) {
		mutex_exit(&rc->rc_mtx);
		return (B_TRUE);
	}

	for (ref = list_head(&rc->rc_list); ref;
	    ref = list_next(&rc->rc_list, ref)) {
		if (ref->ref_holder == holder) {
			mutex_exit(&rc->rc_mtx);
			return (B_FALSE);
		}
	}
	mutex_exit(&rc->rc_mtx);
	return (B_TRUE);
}
#endif	/* ZFS_DEBUG */
