/*-------------------------------------------------------------------------
 *
 * shmem.h
 *	  shared memory management structures
 *
 * Historical note:
 * A long time ago, Postgres' shared memory region was allowed to be mapped
 * at a different address in each process, and shared memory "pointers" were
 * passed around as offsets relative to the start of the shared memory region.
 * That is no longer the case: each process must map the shared memory region
 * at the same address.  This means shared memory pointers can be passed
 * around directly between different processes.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHMEM_H
#define SHMEM_H

#include "storage/spin.h"
#include "utils/hsearch.h"

typedef void (*ShmemInitCallback) (void *arg);
typedef void (*ShmemAttachCallback) (void *arg);

/*
 * ShmemStructDesc describes a named area or struct in shared memory.
 *
 * Shared memory is reserved and allocated in a few phases at postmaster
 * startup, and in EXEC_BACKEND mode, there's some extra work done to "attach"
 * to them at backend startup.  ShmemStructDesc contains all the information
 * needed to manage the lifecycle.
 *
 * 'name' must be filled in before calling ShmemRegisterStruct(); all other
 * fields can be adjusted later during postmaster startup, until the shared
 * memory is allocated and init callback is called. Initialize any optional
 * fields that you don't use to zeros.
 *
 * After registration, the shmem machinery reserves memory for the area, sets
 * '*ptr' to point to the allocation, and calls the callbacks at the right
 * moments.
 */
typedef struct ShmemStructDesc
{
	/*
	 * Name of the shared memory area.  Required, must be unique, and must be
	 * set already before calling ShmemRegisterStruct().
	 */
	const char *name;

	/* Size of the shared memory area.  Required. */
	size_t		size;

	/*
	 * Initialization callback function.  This is called when the shared
	 * memory area is allocated, usually at postmaster startup.  'init_fn_arg'
	 * is an opaque argument passed to the callback.
	 */
	ShmemInitCallback init_fn;
	void	   *init_fn_arg;

	/*
	 * Attachment callback function.  In EXEC_BACKEND mode, this is called at
	 * startup of each backend.  In !EXEC_BACKEND mode, this is only called if
	 * the shared memory area is registered after postmaster startup.  We
	 * never do that in core code, but extensions might.
	 */
	ShmemInitCallback attach_fn;
	void	   *attach_fn_arg;

	/*
	 * Extra space to reserve in the shared memory segment, but it's not part
	 * of the struct itself.  This is used for shared memory hash tables that
	 * can grow beyond the initial size when more buckets are allocated.
	 */
	size_t		extra_size;

	/*
	 * When the shmem area is initialized or attached to, pointer to it is
	 * stored in *ptr.  It usually points to a global variable, used to access
	 * the shared memory area later.  *ptr is set before the init_fn or
	 * attach_fn callback is called.
	 */
	void	  **ptr;
} ShmemStructDesc;

/*
 * Descriptor for a named shared memory hash table.
 *
 * Similar to ShmemStructDesc, but describes a shared memory hash table.  Each
 * hash table is backed by an allocated area, described by 'base_desc', but if
 * 'max_size' is greater than 'init_size', it can also grow beyond the initial
 * allocated area by allocating more hash entries from the global unreserved
 * space.
 */
typedef struct ShmemHashDesc
{
	/*
	 * Name of the shared memory area.  Required.  Must be unique across the
	 * system.
	 */
	const char *name;

	/*
	 * max_size is the estimated maximum number of hashtable entries.  This is
	 * not a hard limit, but the access efficiency will degrade if it is
	 * exceeded substantially (since it's used to compute directory size and
	 * the hash table buckets will get overfull).
	 */
	size_t		max_size;

	/*
	 * init_size is the number of hashtable entries to preallocate.  For a
	 * table whose maximum size is certain, this should be equal to max_size;
	 * that ensures that no run-time out-of-shared-memory failures can occur.
	 */
	size_t		init_size;

	/*
	 * Hash table options passed to hash_create()
	 *
	 * hash_info and hash_flags must specify at least the entry sizes and key
	 * comparison semantics (see hash_create()).  Flag bits and values
	 * specific to shared-memory hash tables are added implicitly in
	 * ShmemRegisterHash(), except that callers may choose to specify
	 * HASH_PARTITION and/or HASH_FIXED_SIZE.
	 */
	HASHCTL		hash_info;
	int			hash_flags;

	/*
	 * When the hash table is initialized or attached to, pointer to its
	 * backend-private handle is stored in *ptr.  It usually points to a
	 * global variable, used to access the hash table later.
	 */
	HTAB	  **ptr;

	/*
	 * Descriptor for the underlying "area".  Callers of ShmemRegisterHash()
	 * do not need to touch this, it is filled in by ShmemRegisterHash() based
	 * on the hash table parameters.
	 */
	ShmemStructDesc base_desc;
} ShmemHashDesc;

/* shmem.c */
extern PGDLLIMPORT slock_t *ShmemLock;
typedef struct PGShmemHeader PGShmemHeader; /* avoid including
											 * storage/pg_shmem.h here */
extern void ResetShmemAllocator(void);
extern void InitShmemAllocator(PGShmemHeader *seghdr);
#ifdef EXEC_BACKEND
extern void AttachShmemAllocator(PGShmemHeader *seghdr);
#endif
extern void *ShmemAlloc(Size size);
extern void *ShmemAllocNoError(Size size);
extern bool ShmemAddrIsValid(const void *addr);

extern bool ShmemRegisterStruct(ShmemStructDesc *desc);
extern bool ShmemRegisterHash(ShmemHashDesc *desc);

/* legacy shmem allocation functions */
extern HTAB *ShmemInitHash(const char *name, int64 init_size, int64 max_size,
						   HASHCTL *infoP, int hash_flags);
extern void *ShmemInitStruct(const char *name, Size size, bool *foundPtr);

extern size_t ShmemRegisteredSize(void);
extern void ShmemInitRegistered(void);
#ifdef EXEC_BACKEND
extern void ShmemAttachRegistered(void);
#endif

extern Size add_size(Size s1, Size s2);
extern Size mul_size(Size s1, Size s2);

extern PGDLLIMPORT Size pg_get_shmem_pagesize(void);

/* ipci.c */
extern void RequestAddinShmemSpace(Size size);

#endif							/* SHMEM_H */
