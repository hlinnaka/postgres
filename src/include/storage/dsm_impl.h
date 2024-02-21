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

/* A "name" for a dynamic shared memory segment. */
typedef uint32 dsm_handle;

/* Sentinel value to use for invalid DSM handles. */
#define DSM_HANDLE_INVALID ((dsm_handle) 0)

/* All the shared-memory operations we know about. */
typedef enum
{
	DSM_OP_CREATE,
	DSM_OP_ATTACH,
	DSM_OP_DETACH,
	DSM_OP_DESTROY,
} dsm_op;

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

/* Create, attach to, detach from, resize, or destroy a segment. */
extern bool dsm_impl_op(dsm_op op, dsm_handle handle, Size request_size,
						dsm_impl_private *impl_private, void **mapped_address, Size *mapped_size,
						int elevel);

/* Implementation-dependent actions required to keep segment until shutdown. */
extern void dsm_impl_pin_segment(dsm_handle handle, dsm_impl_private impl_private,
								 dsm_impl_private_pm_handle *impl_private_pm_handle);
extern void dsm_impl_unpin_segment(dsm_handle handle, dsm_impl_private_pm_handle *pm_handle);

#endif							/* DSM_IMPL_H */
