/*-------------------------------------------------------------------------
 *
 * dsm_impl.c
 *	  manage dynamic shared memory segments
 *
 * This file provides low-level APIs for creating and destroying shared
 * memory segments using several different possible techniques.  We refer
 * to these segments as dynamic because they can be created, altered, and
 * destroyed at any point during the server life cycle.  This is unlike
 * the main shared memory segment, of which there is always exactly one
 * and which is always mapped at a fixed address in every PostgreSQL
 * background process.
 *
 * Because not all systems provide the same primitives in this area, nor
 * do all primitives behave the same way on all systems, we provide
 * several implementations of this facility.  Many systems implement
 * POSIX shared memory (shm_open etc.), which is well-suited to our needs
 * in this area, with the exception that shared memory identifiers live
 * in a flat system-wide namespace, raising the uncomfortable prospect of
 * name collisions with other processes (including other copies of
 * PostgreSQL) running on the same system.  Some systems only support
 * the older System V shared memory interface (shmget etc.) which is
 * also usable; however, the default allocation limits are often quite
 * small, and the namespace is even more restricted.
 *
 * We also provide an mmap-based shared memory implementation.  This may
 * be useful on systems that provide shared memory via a special-purpose
 * filesystem; by opting for this implementation, the user can even
 * control precisely where their shared memory segments are placed.  It
 * can also be used as a fallback for systems where shm_open and shmget
 * are not available or can't be used for some reason.  Of course,
 * mapping a file residing on an actual spinning disk is a fairly poor
 * approximation for shared memory because writeback may hurt performance
 * substantially, but there should be few systems where we must make do
 * with such poor tools.
 *
 * As ever, Windows requires its own implementation.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/dsm_impl.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/dsm_impl.h"
#include "utils/guc_hooks.h"

const struct config_enum_entry dynamic_shared_memory_options[] = {
#ifdef USE_DSM_POSIX
	{"posix", DSM_IMPL_POSIX, false},
#endif
#ifdef USE_DSM_SYSV
	{"sysv", DSM_IMPL_SYSV, false},
#endif
#ifdef USE_DSM_WINDOWS
	{"windows", DSM_IMPL_WINDOWS, false},
#endif
#ifdef USE_DSM_MMAP
	{"mmap", DSM_IMPL_MMAP, false},
#endif
	{NULL, 0, false}
};

/* Implementation selector. */
int			dynamic_shared_memory_type = DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE;

/* Amount of space reserved for DSM segments in the main area. */
int			min_dynamic_shared_memory;

const dsm_impl_ops *dsm_impl;

/*
 * Implementation-specific actions that must be performed when a segment is to
 * be preserved even when no backend has it attached.
 *
 * Except on Windows, we don't need to do anything at all.  But since Windows
 * cleans up segments automatically when no references remain, we duplicate
 * the segment handle into the postmaster process.  The postmaster needn't
 * do anything to receive the handle; Windows transfers it automatically.
 */
void
dsm_impl_noop_pin_segment(dsm_impl_handle handle, dsm_impl_private impl_private,
						  dsm_impl_private_pm_handle *pm_handle)
{
}

/*
 * Implementation-specific actions that must be performed when a segment is no
 * longer to be preserved, so that it will be cleaned up when all backends
 * have detached from it.
 *
 * Except on Windows, we don't need to do anything at all.  For Windows, we
 * close the extra handle that dsm_impl_pin_segment created in the
 * postmaster's process space.
 */
void
dsm_impl_noop_unpin_segment(dsm_impl_handle handle, dsm_impl_private_pm_handle pm_handle)
{
}

int
errcode_for_dynamic_shared_memory(void)
{
	if (errno == EFBIG || errno == ENOMEM)
		return errcode(ERRCODE_OUT_OF_MEMORY);
	else
		return errcode_for_file_access();
}

void
assign_dynamic_shared_memory_type(int new_dynamic_shared_memory_type, void *extra)
{
	switch (new_dynamic_shared_memory_type)
	{
#ifdef USE_DSM_POSIX
		case DSM_IMPL_POSIX:
			dsm_impl = &dsm_impl_posix_ops;
			break;
#endif
#ifdef USE_DSM_SYSV
		case DSM_IMPL_SYSV:
			dsm_impl = &dsm_impl_sysv_ops;
			break;
#endif
#ifdef USE_DSM_WINDOWS
		case DSM_IMPL_WINDOWS:
			dsm_impl = &dsm_impl_windows_ops;
			break;
#endif
#ifdef USE_DSM_MMAP
		case DSM_IMPL_MMAP:
			dsm_impl = &dsm_impl_mmap_ops;
			break;
#endif
		default:
			elog(ERROR, "unexpected dynamic shared memory type: %d",
				 new_dynamic_shared_memory_type);
	}
}
