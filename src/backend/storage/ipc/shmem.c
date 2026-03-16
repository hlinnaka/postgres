/*-------------------------------------------------------------------------
 *
 * shmem.c
 *	  create shared memory and initialize shared memory data structures.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/shmem.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * POSTGRES processes share one or more regions of shared memory.
 * The shared memory is created by a postmaster and is inherited
 * by each backend via fork() (or, in some ports, via other OS-specific
 * methods).  The routines in this file are used for allocating and
 * binding to shared memory data structures.
 *
 * Two kinds of shared memory data structures are handled by this module:
 * fixed-size structures and hash tables.  Fixed-size structures contain
 * things like variables shared between all backend processes.  Hash tables
 * have a fixed maximum size, but their actual size can vary dynamically.
 * When entries are added to the table, more space is allocated.  Each shared
 * data structure and hash has a string name to identify it, specified in the
 * descriptor when its registered.
 *
 * Shared memory structs and hash tables should not be allocated after
 * postmaster startup, although we do allow small allocations later for the
 * benefit of extension modules that loaded after startup.  Despite that
 * allowance, extensions that need shared memory should be added in
 * shared_preload_libraries, because the allowance is quite small and there is
 * no guarantee that any memory is available after startup.
 *
 * Nowadays, there is also third way to allocate shared memory called Dynamic
 * Shared Memory.  See dsm.c for that facility.  One big difference between
 * traditional shared memory handled by shmem.c and dynamic shared memory is
 * that traditional shared memory areas are mapped to the same address in all
 * processes, so you can use normal pointers in shared memory structs.  With
 * Dynamic Shared Memory, you must use offsets or DSA pointers instead.
 *
 * Shared memory managed by shmem.c can never be freed, once allocated.  Each
 * hash table has its own free list, so hash buckets can be reused when an
 * item is deleted.  However, if one hash table grows very large and then
 * shrinks, its space cannot be redistributed to other tables.  We could build
 * a simple hash bucket garbage collector if need be.  Right now, it seems
 * unnecessary.
 *
 * Usage
 * -----
 *
 * To allocate a shared memory area, fill in the name, size, and any other
 * options in ShmemStructDesc, and call ShmemRegisterStruct().  Leave any
 * unused fields as zeros.
 *
 *	typedef struct MyShmemData {
 *		...
 *	} MyShmemData;
 *
 *	static MyShmemData *MyShmem;
 *
 *	static void my_shmem_init(void *arg);
 *
 *	static ShmemStructDesc MyShmemDesc = {
 *		.name = "My shmem area",
 *		.size = sizeof(MyShmemData),
 *		.init_fn = my_shmem_init,
 *		.ptr = &MyShmem,
 *	};
 *
 * In the subsystem's initialization code (or in _PG_init() in extensions),
 * call ShmemRegisterStruct(&MyShmemDesc).
 *
 * Lifecycle
 * ---------
 *
 * RegisterShmemStructs() is called at postmaster startup before calculating
 * the size of the global shared memory segment.  Once all the registrations
 * have been done, postmaster calls ShmemRegisteredSize() to add up the sizes
 * of all the registered areas.  After allocating the shared memory segment,
 * postmaster calls ShmemInitRegistered(), which calls the init_fn callback,
 * if any, of each registered area, in the order that they were registered.
 *
 * In standard Unix-ish environments, individual backends do not need to
 * re-establish their local pointers into shared memory, because they inherit
 * correct values of those variables via fork() from the postmaster.  However,
 * this does not work in the EXEC_BACKEND case.  In ports using EXEC_BACKEND,
 * backend startup also calls RegisterShmemStructs(), followed by
 * ShmemAttachRegistered(), which re-establishes the pointer variables
 * (*ShmemStructDesc->ptr), and calls the attach_fn callback, if any, for
 * additional per-backend setup.
 *
 * Legacy ShmemInitStruct()/ShmemInitHash() functions
 * --------------------------------------------------
 *
 * ShmemInitStruct()/ShmemInitHash() is another way of registring shmem areas.
 * It pre-dates the ShmemRegisterStruct()/ShmemRegisterHash() functions, and
 * should not be used in new code, but as of this writing it is still widely
 * used in extensions.
 *
 * To allocate a shmem area with ShmemInitStruct(), you need to separately
 * register the size needed for the area by calling RequestAddinShmemSpace()
 * from the extension's shmem_request_hook, and allocate the area by calling
 * ShmemInitStruct() from the extension's shmem_startup_hook.  There are no
 * init/attach callbacks.  Instead, the caller of ShmemInitStruct() must check
 * the return status of ShmemInitStruct() and initialize the struct if it was
 * not previously initialized.
 *
 * Calling ShmemAlloc() directly
 * -----------------------------
 *
 * There's a more low-level way of allocating shared memory too: you can call
 * ShmemAlloc() directly.  It's used to implement the higher level mechanisms,
 * and should generally not be called directly.
 */

#include "postgres.h"

#include "common/int.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_numa.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"

/*
 * Array of registered shared memory areas.
 *
 * This is in process private memory, although on Unix-like systems, we expect
 * all the registrations to happen at postmaster startup time and be inherited
 * by all the child processes via fork().  Extensions may register additional
 * areas after startup, but only areas registered at postmaster startup are
 * included in the estimate for the total memory needed for shared memory.  If
 * any non-trivial allocations are made after startup, there might not be
 * enough shared memory available.
 */
static struct
{
	ShmemStructDesc *desc;		/* registered descriptor */
	bool		legacy;			/* legacy ShmemInitStruct/Hash entry? */
} *registered_shmem_areas;
static int	num_registered_shmem_areas = 0;
static int	max_registered_shmem_areas = 0; /* allocated size of the array */

/* estimated size of registered_shmem_areas (not a hard limit) */
#define INITIAL_REGISTRY_SIZE		 (64)

/*
 * This is the first data structure stored in the shared memory segment, at
 * the offset that PGShmemHeader->content_offset points to.  Allocations by
 * ShmemAlloc() are carved out of the space after this.
 *
 * For the base pointer and the total size of the shmem segment, we rely on
 * the PGShmemHeader.
 */
typedef struct ShmemAllocatorData
{
	Size		free_offset;	/* offset to first free space from ShmemBase */
	HASHHDR    *index;			/* location of ShmemIndex */

	/* protects shared memory and LWLock allocation */
	slock_t		shmem_lock;
} ShmemAllocatorData;

static bool ShmemRegisterStructInternal(ShmemStructDesc *desc, bool legacy);
static bool ShmemRegisterHashInternal(ShmemHashDesc *desc, bool legacy);
static void *ShmemAllocRaw(Size size, Size *allocated_size);

static void shmem_hash_init(void *arg);
static void shmem_hash_attach(void *arg);

/* shared memory global variables */

static PGShmemHeader *ShmemSegHdr;	/* shared mem segment header */
static void *ShmemBase;			/* start address of shared memory */
static void *ShmemEnd;			/* end+1 address of shared memory */

static ShmemAllocatorData *ShmemAllocator;
slock_t    *ShmemLock;			/* points to ShmemAllocator->shmem_lock */

static bool shmem_initialized = false;

/*
 * ShmemIndex is a global directory of shmem areas, itself also stored in the
 * shared memory.
 */
static HTAB *ShmemIndex;

 /* max size of data structure string name */
#define SHMEM_INDEX_KEYSIZE		 (48)

/*
 * # of additional entries to reserve in the shmem index table, for allocations
 * after postmaster startup (not a hard limit)
 */
#define SHMEM_INDEX_ADDITIONAL_SIZE		 (64)

/* this is a hash bucket in the shmem index table */
typedef struct
{
	char		key[SHMEM_INDEX_KEYSIZE];	/* string name */
	void	   *location;		/* location in shared mem */
	Size		size;			/* # bytes requested for the structure */
	Size		allocated_size; /* # bytes actually allocated */
} ShmemIndexEnt;

/* To get reliable results for NUMA inquiry we need to "touch pages" once */
static bool firstNumaTouch = true;

Datum		pg_numa_available(PG_FUNCTION_ARGS);

/*
 *	ShmemRegisterStruct() --- register a shared memory struct
 *
 * Subsystems call this to register their shared memory needs.  That should be
 * done early in postmaster startup, before the shared memory segment has been
 * created, so that the size can be included in the estimate for total amount
 * of shared memory needed.  We set aside a small amount of memory for
 * allocations that happen later, for the benefit of non-preloaded extensions,
 * but that should not be relied upon.
 *
 * In core subsystems, each subsystem's registration function is called from
 * RegisterShmemStructs().  In extensions, this should be called from the
 * _PG_init() function.  In EXEC_BACKEND mode, this also needs to be called in
 * each child process, to reattach and set the pointer to the shared memory
 * area, usually in a global variable.  Calling this from the _PG_init()
 * initializer takes care of that too.
 *
 * When called during postmaster startup, before the shared memory has been
 * allocated, the function merely remembers the registered descriptor, but the
 * descriptor may still be changed later, until the shared memory segment has
 * been allocated.  That means that an extension may still modify the
 * already-registered descriptor in the shmem_request_hook.  A common example
 * of when that's useful is when the size depends on MaxBackends: you can
 * leave the size empty in the ShmemRegisterStruct() call and fill it later in
 * the shmem_request_hook.
 *
 * Returns true if the struct was already initialized in shared memory and we
 * merely attached to it.
 */
bool
ShmemRegisterStruct(ShmemStructDesc *desc)
{
	return ShmemRegisterStructInternal(desc, false);
}

static bool
ShmemRegisterStructInternal(ShmemStructDesc *desc, bool legacy)
{
	bool		found;

	/* Check that it's not already registered in this process */
	for (int i = 0; i < num_registered_shmem_areas; i++)
	{
		ShmemStructDesc *existing = registered_shmem_areas[i].desc;

		if (strcmp(existing->name, desc->name) == 0)
			ereport(ERROR,
					(errmsg("shared memory struct \"%s\" is already registered",
							desc->name),
					 errbacktrace()));
	}

	/* desc->ptr can be non-NULL when re-initializing after crash */
	if (!IsUnderPostmaster && desc->ptr)
		*desc->ptr = NULL;

	/* Add the descriptor to the array, growing the array if needed */
	if (num_registered_shmem_areas == max_registered_shmem_areas)
	{
		int			new_size;

		if (registered_shmem_areas)
		{
			new_size = max_registered_shmem_areas * 2;
			registered_shmem_areas = repalloc(registered_shmem_areas,
											  new_size * sizeof(*registered_shmem_areas));
		}
		else
		{
			new_size = INITIAL_REGISTRY_SIZE;
			registered_shmem_areas = MemoryContextAlloc(TopMemoryContext,
														new_size * sizeof(*registered_shmem_areas));
		}
		max_registered_shmem_areas = new_size;
	}
	registered_shmem_areas[num_registered_shmem_areas].desc = desc;
	registered_shmem_areas[num_registered_shmem_areas].legacy = legacy;
	num_registered_shmem_areas++;

	/*
	 * If called after postmaster startup, we need to immediately also
	 * initialize or attach to the area.
	 */
	if (shmem_initialized)
	{
		ShmemIndexEnt *index_entry;

		LWLockAcquire(ShmemIndexLock, LW_EXCLUSIVE);

		/* look it up in the shmem index */
		index_entry = (ShmemIndexEnt *)
			hash_search(ShmemIndex, desc->name, HASH_ENTER_NULL, &found);
		if (!index_entry)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not create ShmemIndex entry for data structure \"%s\"",
							desc->name)));
		}
		if (found)
		{
			/* Already present, just attach to it */
			if (index_entry->size != desc->size)
				elog(ERROR, "shared memory struct \"%s\" is already registered with different size",
					 desc->name);
			if (desc->ptr)
				*desc->ptr = index_entry->location;
			if (desc->attach_fn)
				desc->attach_fn(desc->attach_fn_arg);
		}
		else
		{
			/*
			 * This is the first time.  Initialize it like
			 * ShmemInitRegistered() would
			 */
			size_t		allocated_size;
			void	   *structPtr;

			structPtr = ShmemAllocRaw(desc->size, &allocated_size);
			if (structPtr == NULL)
			{
				/* out of memory; remove the failed ShmemIndex entry */
				hash_search(ShmemIndex, desc->name, HASH_REMOVE, NULL);
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("not enough shared memory for data structure"
								" \"%s\" (%zu bytes requested)",
								desc->name, desc->size)));
			}
			index_entry->size = desc->size;
			index_entry->allocated_size = allocated_size;
			index_entry->location = structPtr;
			if (desc->ptr)
				*desc->ptr = index_entry->location;

			/*
			 * XXX: if this errors out, the area is left in a half-initialized
			 * state
			 */
			if (desc->init_fn)
				desc->init_fn(desc->init_fn_arg);
		}

		LWLockRelease(ShmemIndexLock);
	}
	else
		found = false;

	return found;
}

/*
 *	ShmemRegisteredSize() --- estimate the total size of all registered shared
 *                            memory structures.
 *
 * This is called once at postmaster startup, before the shared memory segment
 * has been created.
 */
size_t
ShmemRegisteredSize(void)
{
	size_t		size;

	/* memory needed for the ShmemIndex */
	size = hash_estimate_size(num_registered_shmem_areas + SHMEM_INDEX_ADDITIONAL_SIZE,
							  sizeof(ShmemIndexEnt));

	/* memory needed for all the registered areas */
	for (int i = 0; i < num_registered_shmem_areas; i++)
	{
		ShmemStructDesc *desc = registered_shmem_areas[i].desc;

		size = add_size(size, desc->size);
		size = add_size(size, desc->extra_size);
	}

	return size;
}

/*
 *	ShmemInitRegistered() --- allocate and initialize pre-registered shared
 *                            memory structures.
 *
 * This is called once at postmaster startup, after the shared memory segment
 * has been created.
 */
void
ShmemInitRegistered(void)
{
	/* Should be called only by the postmaster or a standalone backend. */
	Assert(!IsUnderPostmaster);
	Assert(!shmem_initialized);

	/*
	 * Initialize all the registered memory areas.  There are no concurrent
	 * processes yet, so no need for locking.
	 */
	for (int i = 0; i < num_registered_shmem_areas; i++)
	{
		ShmemStructDesc *desc = registered_shmem_areas[i].desc;
		size_t		allocated_size;
		void	   *structPtr;
		bool		found;
		ShmemIndexEnt *index_entry;

		/* look it up in the shmem index */
		index_entry = (ShmemIndexEnt *)
			hash_search(ShmemIndex, desc->name, HASH_ENTER_NULL, &found);
		if (!index_entry)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not create ShmemIndex entry for data structure \"%s\"",
							desc->name)));
		}
		if (found)
			elog(ERROR, "shared memory struct \"%s\" is already initialized", desc->name);

		/* allocate and initialize it */
		structPtr = ShmemAllocRaw(desc->size, &allocated_size);
		if (structPtr == NULL)
		{
			/* out of memory; remove the failed ShmemIndex entry */
			hash_search(ShmemIndex, desc->name, HASH_REMOVE, NULL);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("not enough shared memory for data structure"
							" \"%s\" (%zu bytes requested)",
							desc->name, desc->size)));
		}
		index_entry->size = desc->size;
		index_entry->allocated_size = allocated_size;
		index_entry->location = structPtr;

		*(desc->ptr) = structPtr;
		if (desc->init_fn)
			desc->init_fn(desc->init_fn_arg);
	}

	shmem_initialized = true;
}

/*
 * Call the attach_fn callbacks of all registered shmem areas
 *
 * This is called at backend startup, in EXEC_BACKEND mode.
 */
#ifdef EXEC_BACKEND
void
ShmemAttachRegistered(void)
{
	/* Must be initializing a (non-standalone) backend */
	Assert(IsUnderPostmaster);
	Assert(ShmemAllocator->index != NULL);

	LWLockAcquire(ShmemIndexLock, LW_SHARED);

	for (int i = 0; i < num_registered_shmem_areas; i++)
	{
		ShmemStructDesc *desc = registered_shmem_areas[i].desc;
		bool		found;
		ShmemIndexEnt *result;

		/* look it up in the shmem index */
		result = (ShmemIndexEnt *)
			hash_search(ShmemIndex, desc->name, HASH_FIND, &found);
		if (!found)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not find ShmemIndex entry for data structure \"%s\"",
							desc->name)));
		}

		if (desc->ptr)
			*desc->ptr = result->location;
		if (desc->attach_fn)
			desc->attach_fn(desc->attach_fn_arg);
	}

	LWLockRelease(ShmemIndexLock);

	shmem_initialized = true;
}
#endif

/*
 *	InitShmemAllocator() --- set up basic pointers to shared memory.
 *
 * Called at postmaster or stand-alone backend startup, to initialize the
 * allocator's data structure in the shared memory segment.  In EXEC_BACKEND,
 * this is also called at backend startup, to set up pointers to the
 * already-initialized data structure.
 */
void
InitShmemAllocator(PGShmemHeader *seghdr)
{
	Size		offset;
	int64		hash_size;
	HASHCTL		info;
	int			hash_flags;
	size_t		size;

	Assert(!shmem_initialized);
	Assert(seghdr != NULL);

	/*
	 * We assume the pointer and offset are MAXALIGN.  Not a hard requirement,
	 * but it's true today and keeps the math below simpler.
	 */
	Assert(seghdr == (void *) MAXALIGN(seghdr));
	Assert(seghdr->content_offset == MAXALIGN(seghdr->content_offset));

	ShmemSegHdr = seghdr;
	ShmemBase = seghdr;
	ShmemEnd = (char *) ShmemBase + seghdr->totalsize;

	/*
	 * Allocations after this point should go through ShmemAlloc, which
	 * expects to allocate everything on cache line boundaries.  Make sure the
	 * first allocation begins on a cache line boundary.
	 */
	offset = CACHELINEALIGN(seghdr->content_offset + sizeof(ShmemAllocatorData));
	if (offset > seghdr->totalsize)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory (%zu bytes requested)",
						offset)));

	ShmemAllocator = (ShmemAllocatorData *) ((char *) seghdr + seghdr->content_offset);
	ShmemLock = &ShmemAllocator->shmem_lock;

#ifndef EXEC_BACKEND
	Assert(!IsUnderPostmaster);
#endif
	if (!IsUnderPostmaster)
	{
		SpinLockInit(&ShmemAllocator->shmem_lock);
		ShmemAllocator->free_offset = offset;
	}

	/*
	 * Create (or attach to) the shared memory index of shmem areas.
	 *
	 * This is the same initialization as ShmemInitHash() does, but we cannot
	 * use ShmemInitHash() here because it relies on ShmemIndex being already
	 * initialized.
	 */
	hash_size = num_registered_shmem_areas + SHMEM_INDEX_ADDITIONAL_SIZE;

	info.keysize = SHMEM_INDEX_KEYSIZE;
	info.entrysize = sizeof(ShmemIndexEnt);
	info.dsize = info.max_dsize = hash_select_dirsize(hash_size);
	info.alloc = ShmemAllocNoError;
	hash_flags = HASH_ELEM | HASH_STRINGS | HASH_SHARED_MEM | HASH_ALLOC | HASH_DIRSIZE;
	if (!IsUnderPostmaster)
	{
		size = hash_get_shared_size(&info, hash_flags);
		ShmemAllocator->index = (HASHHDR *) ShmemAlloc(size);
	}
	else
		hash_flags |= HASH_ATTACH;
	info.hctl = ShmemAllocator->index;
	ShmemIndex = hash_create("ShmemIndex", hash_size, &info, hash_flags);
	Assert(ShmemIndex != NULL);
}

/*
 * Reset the shmem struct registry on postmaster crash restart.
 */
void
ResetShmemAllocator(void)
{
	int			num_retained;

	shmem_initialized = false;

	/*
	 * Shared memory areas will not be registered again after a crash restart.
	 * We don't call RegisterShmemStructs() on crash restart, which would
	 * re-register core subsystems, and we don't reload
	 * shared_preload_libraries either.
	 *
	 * However, we do expect the legacy ShmemInitStruct() function will be
	 * called again for each area, so remove those from the registry.
	 */
	num_retained = 0;
	for (int i = 0; i < num_registered_shmem_areas; i++)
	{
		if (!registered_shmem_areas[i].legacy)
		{
			if (num_retained != i)
				registered_shmem_areas[num_retained] = registered_shmem_areas[i];
			num_retained++;
		}
	}
	num_registered_shmem_areas = num_retained;
}

/*
 * ShmemAlloc -- allocate max-aligned chunk from shared memory
 *
 * Throws error if request cannot be satisfied.
 *
 * Assumes ShmemLock and ShmemSegHdr are initialized.
 */
void *
ShmemAlloc(Size size)
{
	void	   *newSpace;
	Size		allocated_size;

	newSpace = ShmemAllocRaw(size, &allocated_size);
	if (!newSpace)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory (%zu bytes requested)",
						size)));
	return newSpace;
}

/*
 * ShmemAllocNoError -- allocate max-aligned chunk from shared memory
 *
 * As ShmemAlloc, but returns NULL if out of space, rather than erroring.
 */
void *
ShmemAllocNoError(Size size)
{
	Size		allocated_size;

	return ShmemAllocRaw(size, &allocated_size);
}

/*
 * ShmemAllocRaw -- allocate align chunk and return allocated size
 *
 * Also sets *allocated_size to the number of bytes allocated, which will
 * be equal to the number requested plus any padding we choose to add.
 */
static void *
ShmemAllocRaw(Size size, Size *allocated_size)
{
	Size		newStart;
	Size		newFree;
	void	   *newSpace;

	/*
	 * Ensure all space is adequately aligned.  We used to only MAXALIGN this
	 * space but experience has proved that on modern systems that is not good
	 * enough.  Many parts of the system are very sensitive to critical data
	 * structures getting split across cache line boundaries.  To avoid that,
	 * attempt to align the beginning of the allocation to a cache line
	 * boundary.  The calling code will still need to be careful about how it
	 * uses the allocated space - e.g. by padding each element in an array of
	 * structures out to a power-of-two size - but without this, even that
	 * won't be sufficient.
	 */
	size = CACHELINEALIGN(size);
	*allocated_size = size;

	Assert(ShmemSegHdr != NULL);

	SpinLockAcquire(ShmemLock);

	newStart = ShmemAllocator->free_offset;

	newFree = newStart + size;
	if (newFree <= ShmemSegHdr->totalsize)
	{
		newSpace = (char *) ShmemBase + newStart;
		ShmemAllocator->free_offset = newFree;
	}
	else
		newSpace = NULL;

	SpinLockRelease(ShmemLock);

	/* note this assert is okay with newSpace == NULL */
	Assert(newSpace == (void *) CACHELINEALIGN(newSpace));

	return newSpace;
}

/*
 * ShmemAddrIsValid -- test if an address refers to shared memory
 *
 * Returns true if the pointer points within the shared memory segment.
 */
bool
ShmemAddrIsValid(const void *addr)
{
	return (addr >= ShmemBase) && (addr < ShmemEnd);
}

/*
 * ShmemRegisterHash -- Register a shared memory hash table.
 *
 * Similar to ShmemRegisterStruct(), but registers a hash table instead of an
 * opaque area.
 */
bool
ShmemRegisterHash(ShmemHashDesc *desc)
{
	return ShmemRegisterHashInternal(desc, false);
}

static bool
ShmemRegisterHashInternal(ShmemHashDesc *desc, bool legacy)
{
	/*
	 * Hash tables allocated in shared memory have a fixed directory; it can't
	 * grow or other backends wouldn't be able to find it. So, make sure we
	 * make it big enough to start with.
	 *
	 * The shared memory allocator must be specified too.
	 */
	desc->hash_info.dsize = desc->hash_info.max_dsize = hash_select_dirsize(desc->max_size);
	desc->hash_info.alloc = ShmemAllocNoError;
	desc->hash_flags |= HASH_SHARED_MEM | HASH_ALLOC | HASH_DIRSIZE;

	/* Set up the base struct descriptor */
	memset(&desc->base_desc, 0, sizeof(desc->base_desc));
	desc->base_desc.name = desc->name;
	desc->base_desc.size = hash_get_shared_size(&desc->hash_info, desc->hash_flags);
	desc->base_desc.init_fn = shmem_hash_init;
	desc->base_desc.init_fn_arg = desc;
	desc->base_desc.attach_fn = shmem_hash_attach;
	desc->base_desc.attach_fn_arg = desc;

	/*
	 * We need a stable pointer to hold the pointer to the shared memory.  Use
	 * the one passed in the descriptor now.  It will be replaced with the
	 * hash table header by init or attach function.
	 */
	desc->base_desc.ptr = (void **) desc->ptr;

	desc->base_desc.extra_size = hash_estimate_size(desc->max_size, desc->hash_info.entrysize) - desc->base_desc.size;

	return ShmemRegisterStructInternal(&desc->base_desc, legacy);
}

static void
shmem_hash_init(void *arg)
{
	ShmemHashDesc *desc = (ShmemHashDesc *) arg;
	int			hash_flags = desc->hash_flags;

	/* Pass location of hashtable header to hash_create */
	desc->hash_info.hctl = (HASHHDR *) *desc->base_desc.ptr;

	*desc->ptr = hash_create(desc->name, desc->init_size, &desc->hash_info, hash_flags);
}

static void
shmem_hash_attach(void *arg)
{
	ShmemHashDesc *desc = (ShmemHashDesc *) arg;
	int			hash_flags = desc->hash_flags;

	/* attach to it rather than allocate and initialize new space */
	hash_flags |= HASH_ATTACH;

	/* Pass location of hashtable header to hash_create */
	desc->hash_info.hctl = (HASHHDR *) *desc->base_desc.ptr;

	*desc->ptr = hash_create(desc->name, desc->init_size, &desc->hash_info, hash_flags);
}

/*
 * ShmemInitStruct -- Create/attach to a structure in shared memory.
 *
 *		This is called during initialization to find or allocate
 *		a data structure in shared memory.  If no other process
 *		has created the structure, this routine allocates space
 *		for it.  If it exists already, a pointer to the existing
 *		structure is returned.
 *
 *	Returns: pointer to the object.  *foundPtr is set true if the object was
 *		already in the shmem index (hence, already initialized).
 *
 * Note: This is a legacy interface, kept for backwards compatibility with
 * extensions.  Use ShmemRegisterStruct() in new code!
 */
void *
ShmemInitStruct(const char *name, Size size, bool *foundPtr)
{
	ShmemStructDesc *desc;

	Assert(shmem_initialized);

	desc = MemoryContextAllocZero(TopMemoryContext, sizeof(ShmemStructDesc) + sizeof(void *));
	desc->name = name;
	desc->size = size;
	desc->ptr = (void *) (((char *) desc) + sizeof(ShmemStructDesc));

	*foundPtr = ShmemRegisterStructInternal(desc, true);
	Assert(*desc->ptr != NULL);
	return *desc->ptr;
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
 * extensions.  Use ShmemRegisterHash() in new code!
 */
HTAB *
ShmemInitHash(const char *name,		/* table string name for shmem index */
			  int64 init_size,	/* initial table size */
			  int64 max_size,	/* max size of the table */
			  HASHCTL *infoP,	/* info about key and bucket size */
			  int hash_flags)	/* info about infoP */
{
	ShmemHashDesc *desc;

	Assert(shmem_initialized);

	desc = MemoryContextAllocZero(TopMemoryContext, sizeof(ShmemHashDesc) + sizeof(HTAB *));
	desc->name = name;
	desc->init_size = init_size;
	desc->max_size = max_size;
	memcpy(&desc->hash_info, infoP, sizeof(HASHCTL));
	desc->hash_flags = hash_flags;

	desc->ptr = (HTAB **) (((char *) desc) + sizeof(ShmemHashDesc));

	ShmemRegisterHashInternal(desc, true);
	return *desc->ptr;
}

/*
 * Add two Size values, checking for overflow
 */
Size
add_size(Size s1, Size s2)
{
	Size		result;

	if (pg_add_size_overflow(s1, s2, &result))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested shared memory size overflows size_t")));
	return result;
}

/*
 * Multiply two Size values, checking for overflow
 */
Size
mul_size(Size s1, Size s2)
{
	Size		result;

	if (pg_mul_size_overflow(s1, s2, &result))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested shared memory size overflows size_t")));
	return result;
}

/* SQL SRF showing allocated shared memory */
Datum
pg_get_shmem_allocations(PG_FUNCTION_ARGS)
{
#define PG_GET_SHMEM_SIZES_COLS 4
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS hstat;
	ShmemIndexEnt *ent;
	Size		named_allocated = 0;
	Datum		values[PG_GET_SHMEM_SIZES_COLS];
	bool		nulls[PG_GET_SHMEM_SIZES_COLS];

	InitMaterializedSRF(fcinfo, 0);

	LWLockAcquire(ShmemIndexLock, LW_SHARED);

	hash_seq_init(&hstat, ShmemIndex);

	/* output all allocated entries */
	memset(nulls, 0, sizeof(nulls));
	while ((ent = (ShmemIndexEnt *) hash_seq_search(&hstat)) != NULL)
	{
		values[0] = CStringGetTextDatum(ent->key);
		values[1] = Int64GetDatum((char *) ent->location - (char *) ShmemSegHdr);
		values[2] = Int64GetDatum(ent->size);
		values[3] = Int64GetDatum(ent->allocated_size);
		named_allocated += ent->allocated_size;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	/* output shared memory allocated but not counted via the shmem index */
	values[0] = CStringGetTextDatum("<anonymous>");
	nulls[1] = true;
	values[2] = Int64GetDatum(ShmemAllocator->free_offset - named_allocated);
	values[3] = values[2];
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	/* output as-of-yet unused shared memory */
	nulls[0] = true;
	values[1] = Int64GetDatum(ShmemAllocator->free_offset);
	nulls[1] = false;
	values[2] = Int64GetDatum(ShmemSegHdr->totalsize - ShmemAllocator->free_offset);
	values[3] = values[2];
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	LWLockRelease(ShmemIndexLock);

	return (Datum) 0;
}

/*
 * SQL SRF showing NUMA memory nodes for allocated shared memory
 *
 * Compared to pg_get_shmem_allocations(), this function does not return
 * information about shared anonymous allocations and unused shared memory.
 */
Datum
pg_get_shmem_allocations_numa(PG_FUNCTION_ARGS)
{
#define PG_GET_SHMEM_NUMA_SIZES_COLS 3
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS hstat;
	ShmemIndexEnt *ent;
	Datum		values[PG_GET_SHMEM_NUMA_SIZES_COLS];
	bool		nulls[PG_GET_SHMEM_NUMA_SIZES_COLS];
	Size		os_page_size;
	void	  **page_ptrs;
	int		   *pages_status;
	uint64		shm_total_page_count,
				shm_ent_page_count,
				max_nodes;
	Size	   *nodes;

	if (pg_numa_init() == -1)
		elog(ERROR, "libnuma initialization failed or NUMA is not supported on this platform");

	InitMaterializedSRF(fcinfo, 0);

	max_nodes = pg_numa_get_max_node();
	nodes = palloc_array(Size, max_nodes + 2);

	/*
	 * Shared memory allocations can vary in size and may not align with OS
	 * memory page boundaries, while NUMA queries work on pages.
	 *
	 * To correctly map each allocation to NUMA nodes, we need to: 1.
	 * Determine the OS memory page size. 2. Align each allocation's start/end
	 * addresses to page boundaries. 3. Query NUMA node information for all
	 * pages spanning the allocation.
	 */
	os_page_size = pg_get_shmem_pagesize();

	/*
	 * Allocate memory for page pointers and status based on total shared
	 * memory size. This simplified approach allocates enough space for all
	 * pages in shared memory rather than calculating the exact requirements
	 * for each segment.
	 *
	 * Add 1, because we don't know how exactly the segments align to OS
	 * pages, so the allocation might use one more memory page. In practice
	 * this is not very likely, and moreover we have more entries, each of
	 * them using only fraction of the total pages.
	 */
	shm_total_page_count = (ShmemSegHdr->totalsize / os_page_size) + 1;
	page_ptrs = palloc0_array(void *, shm_total_page_count);
	pages_status = palloc_array(int, shm_total_page_count);

	if (firstNumaTouch)
		elog(DEBUG1, "NUMA: page-faulting shared memory segments for proper NUMA readouts");

	LWLockAcquire(ShmemIndexLock, LW_SHARED);

	hash_seq_init(&hstat, ShmemIndex);

	/* output all allocated entries */
	while ((ent = (ShmemIndexEnt *) hash_seq_search(&hstat)) != NULL)
	{
		int			i;
		char	   *startptr,
				   *endptr;
		Size		total_len;

		/*
		 * Calculate the range of OS pages used by this segment. The segment
		 * may start / end half-way through a page, we want to count these
		 * pages too. So we align the start/end pointers down/up, and then
		 * calculate the number of pages from that.
		 */
		startptr = (char *) TYPEALIGN_DOWN(os_page_size, ent->location);
		endptr = (char *) TYPEALIGN(os_page_size,
									(char *) ent->location + ent->allocated_size);
		total_len = (endptr - startptr);

		shm_ent_page_count = total_len / os_page_size;

		/*
		 * If we ever get 0xff (-1) back from kernel inquiry, then we probably
		 * have a bug in mapping buffers to OS pages.
		 */
		memset(pages_status, 0xff, sizeof(int) * shm_ent_page_count);

		/*
		 * Setup page_ptrs[] with pointers to all OS pages for this segment,
		 * and get the NUMA status using pg_numa_query_pages.
		 *
		 * In order to get reliable results we also need to touch memory
		 * pages, so that inquiry about NUMA memory node doesn't return -2
		 * (ENOENT, which indicates unmapped/unallocated pages).
		 */
		for (i = 0; i < shm_ent_page_count; i++)
		{
			page_ptrs[i] = startptr + (i * os_page_size);

			if (firstNumaTouch)
				pg_numa_touch_mem_if_required(page_ptrs[i]);

			CHECK_FOR_INTERRUPTS();
		}

		if (pg_numa_query_pages(0, shm_ent_page_count, page_ptrs, pages_status) == -1)
			elog(ERROR, "failed NUMA pages inquiry status: %m");

		/* Count number of NUMA nodes used for this shared memory entry */
		memset(nodes, 0, sizeof(Size) * (max_nodes + 2));

		for (i = 0; i < shm_ent_page_count; i++)
		{
			int			s = pages_status[i];

			/* Ensure we are adding only valid index to the array */
			if (s >= 0 && s <= max_nodes)
			{
				/* valid NUMA node */
				nodes[s]++;
				continue;
			}
			else if (s == -2)
			{
				/* -2 means ENOENT (e.g. page was moved to swap) */
				nodes[max_nodes + 1]++;
				continue;
			}

			elog(ERROR, "invalid NUMA node id outside of allowed range "
				 "[0, " UINT64_FORMAT "]: %d", max_nodes, s);
		}

		/* no NULLs for regular nodes */
		memset(nulls, 0, sizeof(nulls));

		/*
		 * Add one entry for each NUMA node, including those without allocated
		 * memory for this segment.
		 */
		for (i = 0; i <= max_nodes; i++)
		{
			values[0] = CStringGetTextDatum(ent->key);
			values[1] = Int32GetDatum(i);
			values[2] = Int64GetDatum(nodes[i] * os_page_size);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}

		/* The last entry is used for pages without a NUMA node. */
		nulls[1] = true;
		values[0] = CStringGetTextDatum(ent->key);
		values[2] = Int64GetDatum(nodes[max_nodes + 1] * os_page_size);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	LWLockRelease(ShmemIndexLock);
	firstNumaTouch = false;

	return (Datum) 0;
}

/*
 * Determine the memory page size used for the shared memory segment.
 *
 * If the shared segment was allocated using huge pages, returns the size of
 * a huge page. Otherwise returns the size of regular memory page.
 *
 * This should be used only after the server is started.
 */
Size
pg_get_shmem_pagesize(void)
{
	Size		os_page_size;
#ifdef WIN32
	SYSTEM_INFO sysinfo;

	GetSystemInfo(&sysinfo);
	os_page_size = sysinfo.dwPageSize;
#else
	os_page_size = sysconf(_SC_PAGESIZE);
#endif

	Assert(IsUnderPostmaster);
	Assert(huge_pages_status != HUGE_PAGES_UNKNOWN);

	if (huge_pages_status == HUGE_PAGES_ON)
		GetHugePageSize(&os_page_size, NULL);

	return os_page_size;
}

Datum
pg_numa_available(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(pg_numa_init() != -1);
}
