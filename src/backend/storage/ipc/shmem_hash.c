/*-------------------------------------------------------------------------
 *
 * shmem_hash.c
 *	  hash table implementation in shared memory
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * A shared memory hash table implementation on top of the named, fixed-size
 * shared memory areas managed by shmem.c.  Hash tables have a fixed maximum
 * size, but their actual size can vary dynamically.  When entries are added
 * to the table, more space is allocated.  Each shared data structure and hash
 * has a string name to identify it, specified in its descriptor when its
 * requested.
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/shmem_hash.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/shmem.h"
#include "utils/memutils.h"

/*
 * ShmemRequestHash -- Request a shared memory hash table.
 *
 * Similar to ShmemRequestStruct(), but requests a hash table instead of an
 * opaque area.
 */
void
ShmemRequestHash(ShmemHashDesc *desc, const ShmemRequestHashOpts *options)
{
	ShmemRequestHashOpts *options_copy;
	int64		dirsize;

	Assert(options->name != NULL);

	options_copy = MemoryContextAlloc(TopMemoryContext,
									  sizeof(ShmemRequestHashOpts));
	memcpy(options_copy, options, sizeof(ShmemRequestHashOpts));

	/*
	 * Hash tables allocated in shared memory have a fixed directory; it can't
	 * grow or other backends wouldn't be able to find it. So, make sure we
	 * make it big enough to start with.
	 *
	 * The shared memory allocator must be specified too.
	 */
	dirsize = hash_select_dirsize(options->max_size);
	options_copy->hash_info.dsize = dirsize;
	options_copy->hash_info.max_dsize = dirsize;
	options_copy->hash_info.alloc = ShmemAllocNoError;
	options_copy->hash_flags |= HASH_SHARED_MEM | HASH_ALLOC | HASH_DIRSIZE;

	/*
	 * Create a struct descriptor for the fixed-size area holding the hash
	 * table
	 */
	options_copy->base.name = options->name;
	options_copy->base.size = hash_get_shared_size(&options_copy->hash_info,
												   options_copy->hash_flags);

	/* Reserve extra space for the buckets */
	options_copy->base.extra_size =
		hash_estimate_size(options->max_size, options_copy->hash_info.entrysize) - options_copy->base.size;

	ShmemRequestInternal(&desc->base, &options_copy->base, SHMEM_KIND_HASH);
}

void
shmem_hash_init(ShmemStructDesc *base_desc, const ShmemRequestStructOpts *base_options)
{
	ShmemHashDesc *desc = (ShmemHashDesc *) base_desc;
	ShmemRequestHashOpts *options = (ShmemRequestHashOpts *) base_options;
	int			hash_flags = options->hash_flags;

	options->hash_info.hctl = desc->base.ptr;
	Assert(options->hash_info.hctl != NULL);
	desc->ptr = hash_create(desc->base.name, options->init_size, &options->hash_info, hash_flags);

	if (options->ptr)
		*options->ptr = desc->ptr;
}

void
shmem_hash_attach(ShmemStructDesc *base_desc, const ShmemRequestStructOpts *base_options)
{
	ShmemHashDesc *desc = (ShmemHashDesc *) base_desc;
	ShmemRequestHashOpts *options = (ShmemRequestHashOpts *) base_options;
	int			hash_flags = options->hash_flags;

	/* attach to it rather than allocate and initialize new space */
	hash_flags |= HASH_ATTACH;
	options->hash_info.hctl = desc->base.ptr;
	Assert(options->hash_info.hctl != NULL);
	desc->ptr = hash_create(desc->base.name, options->init_size, &options->hash_info, hash_flags);

	if (options->ptr)
		*options->ptr = desc->ptr;
}


/*
 * ShmemInitHash -- Create and initialize, or attach to, a
 *		shared memory hash table.
 *
 * We assume caller is doing some kind of synchronization
 * so that two processes don't try to create/initialize the same
 * table at once.  (In practice, all creations are done in the postmaster
 * process; child processes should always be attaching to existing tables.)
 *
 * max_size is the estimated maximum number of hashtable entries.  This is
 * not a hard limit, but the access efficiency will degrade if it is
 * exceeded substantially (since it's used to compute directory size and
 * the hash table buckets will get overfull).
 *
 * init_size is the number of hashtable entries to preallocate.  For a table
 * whose maximum size is certain, this should be equal to max_size; that
 * ensures that no run-time out-of-shared-memory failures can occur.
 *
 * *infoP and hash_flags must specify at least the entry sizes and key
 * comparison semantics (see hash_create()).  Flag bits and values specific
 * to shared-memory hash tables are added here, except that callers may
 * choose to specify HASH_PARTITION and/or HASH_FIXED_SIZE.
 *
 * Note: This is a legacy interface, kept for backwards compatibility with
 * extensions.  Use ShmemRequestHash() in new code!
 */
HTAB *
ShmemInitHash(const char *name,		/* table string name for shmem index */
			  int64 init_size,	/* initial table size */
			  int64 max_size,	/* max size of the table */
			  HASHCTL *infoP,	/* info about key and bucket size */
			  int hash_flags)	/* info about infoP */
{
	bool		found;
	void	   *location;

	/*
	 * Hash tables allocated in shared memory have a fixed directory; it can't
	 * grow or other backends wouldn't be able to find it. So, make sure we
	 * make it big enough to start with.
	 *
	 * The shared memory allocator must be specified too.
	 */
	infoP->dsize = infoP->max_dsize = hash_select_dirsize(max_size);
	infoP->alloc = ShmemAllocNoError;
	hash_flags |= HASH_SHARED_MEM | HASH_ALLOC | HASH_DIRSIZE;

	/* look it up in the shmem index */
	location = ShmemInitStruct(name,
							   hash_get_shared_size(infoP, hash_flags),
							   &found);

	/*
	 * if it already exists, attach to it rather than allocate and initialize
	 * new space
	 */
	if (found)
		hash_flags |= HASH_ATTACH;

	/* Pass location of hashtable header to hash_create */
	infoP->hctl = (HASHHDR *) location;

	return hash_create(name, init_size, infoP, hash_flags);
}
