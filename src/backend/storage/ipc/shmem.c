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
 * This module provides facilities to allocate fixed-size structures in shared
 * memory, for things like variables shared between all backend processes.
 * Each such structure has a string name to identify it, specified in the
 * descriptor when it is requested.  shmem_hash.c provides a shared hash table
 * implementation on top of that.
 *
 * Shared memory areas should usually not be allocated after postmaster
 * startup, although we do allow small allocations later for the benefit of
 * extension modules that loaded after startup.  Despite that allowance,
 * extensions that need shared memory should be added in
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
 * To allocate shared memory, you need to register a set of callback functions
 * which handle the lifecycle of the allocation.  In the register_fn
 * callback, fill in a ShmemStructDesc descriptor with the name, size, and any
 * other options, and call ShmemRequestStruct().  Leave any unused fields as
 * zeros.
 *
 *	typedef struct MyShmemData {
 *		...
 *	} MyShmemData;
 *
 *	static MyShmemData *MyShmem;
 *
 *	static void my_shmem_request(void *arg);
 *	static void my_shmem_init(void *arg);
 *
 *  const ShmemCallbacks MyShmemCallbacks = {
 *		.request_fn = my_shmem_request,
 *		.init_fn = my_shmem_init,
 *	};
 *
 *	static void
 *	my_shmem_request(void *arg)
 *	{
 *		static ShmemStructDesc MyShmemDesc = {
 *			.name = "My shmem area",
 *			.size = sizeof(MyShmemData),
 *			.ptr = (void **) &MyShmem,
 *		};
 *
 *		ShmemRequestStruct(&MyShmemDesc);
 *	}
 *
 * In builtin PostgreSQL code, add the callbacks to the list in
 * src/include/storage/subsystemlist.h. In an add-in module, you can register
 * the callbacks by calling RegisterShmemCallbacks(&MyShmemCallbacks) in the
 * extension's _PG_init() function.
 *
 * Lifecycle
 * ---------
 *
 * Initializing shared memory happens in multiple phases. In the first phase,
 * during postmaster startup, all the shmem_request callbacks are called.
 * Only after all all the request callbacks have been called and all the shmem
 * areas have been requested by the ShmemRequestStruct() calls we know how
 * much shared memory we need in total. After that, postmaster allocates
 * global shared memory segment, and calls all the init_fn callbacks to
 * initialize all the requested shmem areas.
 *
 * In standard Unix-ish environments, individual backends do not need to
 * re-establish their local pointers into shared memory, because they inherit
 * correct values of those variables via fork() from the postmaster.  However,
 * this does not work in the EXEC_BACKEND case.  In ports using EXEC_BACKEND,
 * backend startup also calls the shmem_request callbacks to re-establish the
 * knowledge about each shared memory area, sets the pointer variables
 * (*ShmemStructDesc->ptr), and calls the attach_fn callback, if any, for
 * additional per-backend setup.
 *
 * Legacy ShmemInitStruct()/ShmemInitHash() functions
 * --------------------------------------------------
 *
 * ShmemInitStruct()/ShmemInitHash() is another way of registring shmem areas.
 * It pre-dates the ShmemRequestStruct()/ShmemRequestHash() functions, and
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

#include <unistd.h>

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
#include "utils/tuplestore.h"

/*
 * Registered callbacks.
 *
 * During postmaster startup, we accumulate the callbacks from all subsystems
 * in this list.
 *
 * This is in process private memory, although on Unix-like systems, we expect
 * all the registrations to happen at postmaster startup time and be inherited
 * by all the child processes via fork().
 */
static List *registered_shmem_callbacks;

/*
 * In the shmem request phase, all the shmem areas requested with
 * ShmemRequestStruct() are accumulated here.
 */
static List *requested_shmem_areas;

/*
 * Per-process state machine, for sanity checking that we do things in the
 * right order.
 *
 * Postmaster:
 *   INITIAL -> REQUESTING -> INITIALIZING -> DONE
 *
 * Backends in EXEC_BACKEND mode:
 *   INITIAL -> REQUESTING -> ATTACHING -> DONE
 *
 * Late request:
 *   DONE -> REQUESTING -> LATE_ATTACH_OR_INIT -> DONE
 */
static enum
{
	/* Initial state */
	SB_INITIAL,

	/*
	 * When we call the shmem_request callbacks, we enter the SB_REQUESTING
	 * phase. All ShmemRequestStruct calls happen in this state.
	 */
	SB_REQUESTING,

	/*
	 * Postmaster has finished all shmem requests, and is now initializing the
	 * shared memory segment. We are now calling the init_fn callbacks.
	 */
	SB_INITIALIZING,

	/*
	 * A postmaster child process is starting up. The attach_fn callbacks are
	 * called in this state.
	 */
	SB_ATTACHING,

	/* An after-startup allocation or attachment is in progress. */
	SB_LATE_ATTACH_OR_INIT,

	/* Normal state after shmem initialization / attachment */
	SB_DONE,
}			shmem_startup_state;

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

static void *ShmemAllocRaw(Size size, Size *allocated_size);

/* shared memory global variables */

static PGShmemHeader *ShmemSegHdr;	/* shared mem segment header */
static void *ShmemBase;			/* start address of shared memory */
static void *ShmemEnd;			/* end+1 address of shared memory */

static ShmemAllocatorData *ShmemAllocator;
slock_t    *ShmemLock;			/* points to ShmemAllocator->shmem_lock */

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

static bool AttachOrInit(ShmemStructDesc *desc, bool init_allowed, bool attach_allowed);

Datum		pg_numa_available(PG_FUNCTION_ARGS);

/*
 *	ShmemRequestStruct() --- request a named shared memory area
 *
 * Subsystems call this to register their shared memory needs.  This is
 * usually done early in postmaster startup, before the shared memory segment
 * has been created, so that the size can be included in the estimate for
 * total amount of shared memory needed.  We set aside a small amount of
 * memory for allocations that happen later, for the benefit of non-preloaded
 * extensions, but that should not be relied upon.
 *
 * This does not yet allocate the memory, but merely register the need for it.
 * The actual allocation happens later in the postmaster startup sequence.
 *
 * This must be called from a shmem_request callback function, registered with
 * RegisterShmemCallbacks().  This enforces a coding pattern that works the
 * same in normal Unix systems and with EXEC_BACKEND.  In postmaster, the the
 * shmem_request callback is called during startup, but in EXEC_BACKEND mode,
 * it is also called in each backend at backend startup.  By calling the same
 * function in both cases, we ensure that all the shmem areas are registered
 * the same way in all processes.
 */
void
ShmemRequestStruct(ShmemStructDesc *desc)
{
	ListCell *lc;

	if (shmem_startup_state != SB_REQUESTING)
		elog(ERROR, "ShmemRequestStruct can only be called from a shmem_request callback");

	/* Check that it's not already registered in this process */
	foreach(lc, requested_shmem_areas)
	{
		ShmemStructDesc *existing = (ShmemStructDesc *) lfirst(lc);

		if (strcmp(existing->name, desc->name) == 0)
			ereport(ERROR,
					(errmsg("shared memory struct \"%s\" is already registered",
							desc->name)));
	}

	requested_shmem_areas = lappend(requested_shmem_areas, desc);
}

/*
 *	ShmemGetRequestedSize() --- estimate the total size of all registered shared
 *                              memory structures.
 *
 * This is called once at postmaster startup, before the shared memory segment
 * has been created.
 */
size_t
ShmemGetRequestedSize(void)
{
	ListCell   *lc;
	size_t		size;

	/* memory needed for the ShmemIndex */
	size = hash_estimate_size(list_length(requested_shmem_areas) + SHMEM_INDEX_ADDITIONAL_SIZE,
							  sizeof(ShmemIndexEnt));

	/* memory needed for all the requested areas */
	foreach(lc, requested_shmem_areas)
	{
		ShmemStructDesc *desc = (ShmemStructDesc *) lfirst(lc);

		size = add_size(size, desc->size);
		size = add_size(size, desc->extra_size);
	}

	return size;
}

/*
 *	ShmemInitRequested() --- allocate and initialize requested shared memory
 *                            structures.
 *
 * This is called once at postmaster startup, after the shared memory segment
 * has been created.
 */
void
ShmemInitRequested(void)
{
	ListCell	   *lc;

	/* Should be called only by the postmaster or a standalone backend. */
	Assert(!IsUnderPostmaster);
	Assert(shmem_startup_state == SB_INITIALIZING);

	/*
	 * Initialize all the requested memory areas.  There are no concurrent
	 * processes yet, so no need for locking.
	 */
	foreach(lc, requested_shmem_areas)
	{
		ShmemStructDesc *desc = (ShmemStructDesc *) lfirst(lc);

		AttachOrInit(desc, true, false);
	}
	list_free(requested_shmem_areas);
	requested_shmem_areas = NIL;

	/* Call init callbacks */
	foreach(lc, registered_shmem_callbacks)
	{
		const ShmemCallbacks *callbacks = (const ShmemCallbacks *) lfirst(lc);

		if (callbacks->init_fn)
			callbacks->init_fn(callbacks->init_fn_arg);
	}

	shmem_startup_state = SB_DONE;
}

/*
 * Re-establish process private state related to shmem areas.
 *
 * This is called at backend startup in EXEC_BACKEND mode, in every backend.
 */
#ifdef EXEC_BACKEND
void
ShmemAttachRequested(void)
{
	/* Must be initializing a (non-standalone) backend */
	Assert(IsUnderPostmaster);
	Assert(ShmemAllocator->index != NULL);
	Assert(shmem_startup_state == SB_REQUESTING);
	shmem_startup_state = SB_ATTACHING;

	/*
	 * Attach to all the requested memory areas.
	 */
	LWLockAcquire(ShmemIndexLock, LW_SHARED);
	while (!dclist_is_empty(&requested_shmem_areas))
	{
		requested_shmem_area *area = dlist_container(requested_shmem_area, node,
													 dclist_pop_head_node(&requested_shmem_areas));
		ShmemStructDesc *desc = area->desc;

		AttachOrInit(desc, false, true);
	}
	list_free(requested_shmem_areas);
	requested_shmem_areas = NIL;

	/* Call attach callbacks */
	dlist_foreach(iter, &registered_shmem_init_callbacks)
	{
		registered_shmem_init_callback *callback =
			dlist_container(registered_shmem_init_callback, node, iter.cur);

		callback->attach_fn(callback->attach_fn_arg);
	}

	LWLockRelease(ShmemIndexLock);

	shmem_startup_state = SB_DONE;
}
#endif

/*
 * Workhorse to insert or look up a named shmem area in the shared memory
 * index, and initialize or attach to it.
 *
 * If !init_allowed and the entry is not found, throws an error.  If
 * !attach_allowed and the entry is found, throws an error.
 */
static bool
AttachOrInit(ShmemStructDesc *desc, bool init_allowed, bool attach_allowed)
{
	/*
	 * If called after postmaster startup, we need to immediately also
	 * initialize or attach to the area.
	 */
	ShmemIndexEnt *index_entry;
	bool		found;

	/* look it up in the shmem index */
	index_entry = (ShmemIndexEnt *)
		hash_search(ShmemIndex, desc->name,
					init_allowed ? HASH_ENTER_NULL : HASH_FIND, &found);
	if (found)
	{
		/* Already present, just attach to it */
		if (!attach_allowed)
			elog(ERROR, "shared memory struct \"%s\" is already initialized", desc->name);

		if (index_entry->size != desc->size)
			elog(ERROR, "shared memory struct \"%s\" is already registered with different size",
				 desc->name);
		switch (desc->kind)
		{
			case SHMEM_KIND_STRUCT:
				if (desc->ptr)
					*(desc->ptr) = index_entry->location;
				break;
			case SHMEM_KIND_HASH:
				shmem_hash_attach(desc, index_entry->location);
				break;
		}
	}
	else if (!init_allowed)
	{
		/* attach was requested, but it was not found */
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not find ShmemIndex entry for data structure \"%s\"",
						desc->name)));
	}
	else if (!index_entry)
	{
		/* tried to add it to the hash table, but there was no space */
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create ShmemIndex entry for data structure \"%s\"",
						desc->name)));
	}
	else
	{
		/*
		 * We inserted the entry to the shared memory index. Allocate
		 * requested amount of shared memory for it, and do basic
		 * initializion.
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
		switch (desc->kind)
		{
			case SHMEM_KIND_STRUCT:
				if (desc->ptr)
					*(desc->ptr) = index_entry->location;
				break;
			case SHMEM_KIND_HASH:
				shmem_hash_init(desc, index_entry->location);
				break;
		}
	}

	return found;
}

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

	Assert(seghdr != NULL);

	if (IsUnderPostmaster)
	{
		Assert(shmem_startup_state == SB_INITIAL);
	}
	else
	{
		Assert(shmem_startup_state == SB_REQUESTING);
		shmem_startup_state = SB_INITIALIZING;
	}

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
	hash_size = list_length(requested_shmem_areas) + SHMEM_INDEX_ADDITIONAL_SIZE;

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
 * Reset state on postmaster crash restart.
 */
void
ResetShmemAllocator(void)
{
	Assert(!IsUnderPostmaster);
	shmem_startup_state = SB_INITIAL;
	requested_shmem_areas = NIL;

	/*
	 * Note that we don't clear the registered callbacks.  We will need to call
	 * them again as we restart
	 */
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
 * Register callbacks that define a shared memory area (or multiple areas).
 *
 * The system will call the callbacks at different stages of postmaster or
 * backend startup, to allocate and initialize the area.
 *
 * This is normally called early during postmaster startup, but if the
 * SHMEM_ALLOW_AFTER_STARTUP is set, this can also be used after startup,
 * although after startup there's no guarantee that there's enough shared
 * memory available.  When called after startup, this immediately calls the
 * right callbacks depending on whether another backend had already
 * initialized the area.
 */
void
RegisterShmemCallbacks(const ShmemCallbacks *callbacks)
{
	if (IsUnderPostmaster && !process_shared_preload_libraries_in_progress)
	{
		/* After-startup initialization */
		ListCell   *lc;
		bool		found = false;

		if ((callbacks->flags & SHMEM_ALLOW_AFTER_STARTUP) == 0)
			elog(ERROR, "FIXME: not allowed after startup");

		Assert(requested_shmem_areas == NIL);
		Assert(shmem_startup_state == SB_DONE);
		shmem_startup_state = SB_REQUESTING;
		if (callbacks->request_fn)
			callbacks->request_fn(callbacks->request_fn_arg);
		shmem_startup_state = SB_LATE_ATTACH_OR_INIT;

		LWLockAcquire(ShmemIndexLock, LW_EXCLUSIVE);

		foreach(lc, requested_shmem_areas)
		{
			ShmemStructDesc *desc = (ShmemStructDesc *) lfirst(lc);

			found = AttachOrInit(desc, true, true);
		}

		/*
		 * FIXME: What to do if multiple shmem areas were requested, and some
		 * of them are already initialized but not all?
		 */
		if (found)
		{
			if (callbacks->attach_fn)
				callbacks->attach_fn(callbacks->attach_fn_arg);
		}
		else
		{
			if (callbacks->init_fn)
				callbacks->init_fn(callbacks->init_fn_arg);
		}

		LWLockRelease(ShmemIndexLock);
		shmem_startup_state = SB_DONE;
		return;
	}

	registered_shmem_callbacks = lappend(registered_shmem_callbacks,
										 (void *) callbacks);
}

/*
 * Call all shmem request callbacks.
 */
void
ShmemCallRequestCallbacks(void)
{
	ListCell *lc;

	Assert(shmem_startup_state == SB_INITIAL);
	shmem_startup_state = SB_REQUESTING;

	foreach(lc, registered_shmem_callbacks)
	{
		const ShmemCallbacks *callbacks = (const ShmemCallbacks *) lfirst(lc);

		if (callbacks->request_fn)
			callbacks->request_fn(callbacks->request_fn_arg);
	}
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
 * extensions.  Use ShmemRequestStruct() in new code!
 */
void *
ShmemInitStruct(const char *name, Size size, bool *foundPtr)
{
	void	   *ptr = NULL;
	ShmemStructDesc desc = {
		.name = name,
		.kind = SHMEM_KIND_STRUCT,
		.size = size,
		.ptr = &ptr,
	};

	Assert(shmem_startup_state == SB_DONE);

	/* look it up immediately */
	LWLockAcquire(ShmemIndexLock, LW_EXCLUSIVE);
	*foundPtr = AttachOrInit(&desc, true, true);
	LWLockRelease(ShmemIndexLock);

	Assert(ptr != NULL);
	return ptr;
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
