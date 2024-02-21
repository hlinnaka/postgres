/*-------------------------------------------------------------------------
 *
 * dsm_impl.h
 *	  low-level dynamic shared memory primitives
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/dsm_impl.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DSM_IMPL_H
#define DSM_IMPL_H

/*
 * Note, must be meaningful even across restarts
 */
typedef uint32 dsm_impl_handle;

#define DSM_IMPL_HANDLE_INVALID 0

/* Dynamic shared memory implementations. */
#define DSM_IMPL_POSIX			1
#define DSM_IMPL_SYSV			2
#define DSM_IMPL_WINDOWS		3
#define DSM_IMPL_MMAP			4

/*
 * Determine which dynamic shared memory implementations will be supported
 * on this platform, and which one will be the default.
 */
#ifdef WIN32
#define USE_DSM_WINDOWS
#define DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE		DSM_IMPL_WINDOWS
#else
#ifdef HAVE_SHM_OPEN
#define USE_DSM_POSIX
#define DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE		DSM_IMPL_POSIX
#endif
#define USE_DSM_SYSV
#ifndef DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE
#define DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE		DSM_IMPL_SYSV
#endif
#define USE_DSM_MMAP
#endif

/* GUC. */
extern PGDLLIMPORT int dynamic_shared_memory_type;
extern PGDLLIMPORT int min_dynamic_shared_memory;

/*
 * Directory for on-disk state.
 *
 * This is used by all implementations for crash recovery and by the mmap
 * implementation for storage.
 */
#define PG_DYNSHMEM_DIR					"pg_dynshmem"
#define PG_DYNSHMEM_MMAP_FILE_PREFIX	"mmap."

/*
 * When a segment is created or attached, the caller provides this space to
 * hold implementation-specific information about the attachment. It is opaque
 * to the caller, and is passed back to the implementation when detaching.
 */
typedef uintptr_t dsm_impl_private;

/*
 * Similar caller-provided space for implementation-specific information held
 * when a segment is pinned.
 */
typedef uintptr_t dsm_impl_private_pm_handle;

/* All the shared-memory operations we know about. */
typedef struct
{
	/*
	 * Create a new DSM segment, and map it to the current process's address
	 * space.
	 *
	 * Note: the mapped_size can differ from requested size (currently only
	 * Windows)
	 *
	 */
	dsm_impl_handle(*create) (Size request_size, dsm_impl_private *impl_private,
							  void **mapped_address, int elevel);

	/*
	 * Map an existing DSM segment to current process's address space.
	 */
	bool		(*attach) (dsm_impl_handle handle, dsm_impl_private *impl_private,
						   void **mapped_address, Size *mapped_size, int elevel);

	/*
	 * Unmap
	 */
	bool		(*detach) (dsm_impl_handle handle, dsm_impl_private impl_private,
						   void *mapped_address, Size mapped_size, int elevel);

	/*
	 * Destroy a DSM segment.
	 *
	 * Note: must detach first
	 */
	bool		(*destroy) (dsm_impl_handle handle, int elevel);

	/*
	 * Implementation-specific actions that must be performed when a segment
	 * is to be preserved even when no backend has it attached.
	 */
	void		(*pin_segment) (dsm_impl_handle handle, dsm_impl_private impl_private,
								dsm_impl_private_pm_handle *pm_handle);

	/*
	 * Implementation-specific actions that must be performed when a segment
	 * is no longer to be preserved, so that it will be cleaned up when all
	 * backends have detached from it.
	 */
	void		(*unpin_segment) (dsm_impl_handle handle, dsm_impl_private_pm_handle pm_handle);
} dsm_impl_ops;

extern PGDLLIMPORT const dsm_impl_ops *dsm_impl;

#ifdef USE_DSM_POSIX
extern PGDLLIMPORT const dsm_impl_ops dsm_impl_posix_ops;
#endif
#ifdef USE_DSM_SYSV
extern PGDLLIMPORT const dsm_impl_ops dsm_impl_sysv_ops;
#endif
#ifdef USE_DSM_WINDOWS
extern PGDLLIMPORT const dsm_impl_ops dsm_impl_windows_ops;
#endif
#ifdef USE_DSM_MMAP
extern PGDLLIMPORT const dsm_impl_ops dsm_impl_mmap_ops;
#endif

extern void dsm_impl_noop_pin_segment(dsm_impl_handle handle, dsm_impl_private impl_private,
									  dsm_impl_private_pm_handle *pm_handle);
extern void dsm_impl_noop_unpin_segment(dsm_impl_handle handle, dsm_impl_private_pm_handle pm_handle);

extern int	errcode_for_dynamic_shared_memory(void);

#endif							/* DSM_IMPL_H */
