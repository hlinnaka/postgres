/*-------------------------------------------------------------------------
 *
 * pmchild.c
 *	  Functions for keeping track of postmaster child processes.
 *
 * Keep track of all child processes, so that when a process exits, we know
 * kind of a process it was and can clean up accordingly.  Every child process
 * is allocated a PMChild struct, from a fixed pool of structs.  The size of
 * the pool is determined by various settings that configure how many worker
 * processes and backend connections are allowed, i.e. autovacuum_max_workers,
 * max_worker_processes, max_wal_senders, and max_connections.
 *
 * The structures and functions in this file are private to the postmaster
 * process.  But note that there is an array in shared memory, managed by
 * pmsignal.c, that mirrors this.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/pmchild.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"

/*
 * Freelists for different kinds of child processes.  We maintain separate
 * pools for them, so that launching a lot of backends cannot exchaust all the
 * slots, and prevent autovacuum or an aux process from launching.
 */
static dlist_head freeBackendList;
static dlist_head freeAutoVacWorkerList;
static dlist_head freeBgWorkerList;
static dlist_head freeAuxList;

/*
 * List of active child processes.  This includes dead-end children.
 */
dlist_head	ActiveChildList;

/*
 * MaxLivePostmasterChildren
 *
 * This reports the number postmaster child processes that can be active.  It
 * includes all children except for dead_end children.  This allows the array
 * in shared memory (PMChildFlags) to have a fixed maximum size.
 */
int
MaxLivePostmasterChildren(void)
{
	int			n = 0;

	/* We know exactly how mamy worker and aux processes can be active */
	n += autovacuum_max_workers;
	n += max_worker_processes;
	n += NUM_AUXILIARY_PROCS;

	/*
	 * We allow more connections here than we can have backends because some
	 * might still be authenticating; they might fail auth, or some existing
	 * backend might exit before the auth cycle is completed.  The exact
	 * MaxBackends limit is enforced when a new backend tries to join the
	 * shared-inval backend array.
	 */
	n += 2 * (MaxConnections + max_wal_senders);

	return n;
}

static void
init_slot(PMChild *pmchild, int slotno, dlist_head *freelist)
{
	pmchild->pid.pid = 0;
	pmchild->child_slot = slotno + 1;
	pmchild->bkend_type = B_INVALID;
	pmchild->rw = NULL;
	pmchild->bgworker_notify = false;
	dlist_push_tail(freelist, &pmchild->elem);
}

/*
 * Initialize at postmaster startup
 */
void
InitPostmasterChildSlots(void)
{
	int			num_pmchild_slots;
	int			slotno;
	PMChild    *slots;

	dlist_init(&freeBackendList);
	dlist_init(&freeAutoVacWorkerList);
	dlist_init(&freeBgWorkerList);
	dlist_init(&freeAuxList);
	dlist_init(&ActiveChildList);

	num_pmchild_slots = MaxLivePostmasterChildren();

	slots = palloc(num_pmchild_slots * sizeof(PMChild));

	slotno = 0;
	for (int i = 0; i < 2 * (MaxConnections + max_wal_senders); i++)
	{
		init_slot(&slots[slotno], slotno, &freeBackendList);
		slotno++;
	}
	for (int i = 0; i < autovacuum_max_workers; i++)
	{
		init_slot(&slots[slotno], slotno, &freeAutoVacWorkerList);
		slotno++;
	}
	for (int i = 0; i < max_worker_processes; i++)
	{
		init_slot(&slots[slotno], slotno, &freeBgWorkerList);
		slotno++;
	}
	for (int i = 0; i < NUM_AUXILIARY_PROCS; i++)
	{
		init_slot(&slots[slotno], slotno, &freeAuxList);
		slotno++;
	}
	Assert(slotno == num_pmchild_slots);
}

/* Return the appropriate free-list for the given backend type */
static dlist_head *
GetFreeList(BackendType btype)
{
	switch (btype)
	{
		case B_BACKEND:
		case B_BG_WORKER:
		case B_WAL_SENDER:
		case B_SLOTSYNC_WORKER:
			return &freeBackendList;
		case B_AUTOVAC_WORKER:
			return &freeAutoVacWorkerList;

			/*
			 * Auxiliary processes.  There can be only one of each of these
			 * running at a time.
			 */
		case B_AUTOVAC_LAUNCHER:
		case B_ARCHIVER:
		case B_BG_WRITER:
		case B_CHECKPOINTER:
		case B_STARTUP:
		case B_WAL_RECEIVER:
		case B_WAL_SUMMARIZER:
		case B_WAL_WRITER:
			return &freeAuxList;

			/*
			 * Logger is not connected to shared memory, and does not have a
			 * PGPROC entry, but we still allocate a child slot for it.
			 */
		case B_LOGGER:
			return &freeAuxList;

		case B_STANDALONE_BACKEND:
		case B_INVALID:
		case B_DEAD_END_BACKEND:
			break;
	}
	elog(ERROR, "unexpected BackendType: %d", (int) btype);
	return NULL;
}

/*
 * Allocate a PMChild entry for a backend of given type.
 *
 * The entry is taken from the right pool.
 *
 * pmchild->child_slot is unique among all active child processes
 */
PMChild *
AssignPostmasterChildSlot(BackendType btype)
{
	dlist_head *freelist;
	PMChild    *pmchild;

	freelist = GetFreeList(btype);

	if (dlist_is_empty(freelist))
		return NULL;

	pmchild = dlist_container(PMChild, elem, dlist_pop_head_node(freelist));
	pmchild->pid.pid = 0;
	pmchild->bkend_type = btype;
	pmchild->rw = NULL;
	pmchild->bgworker_notify = true;

	/*
	 * pmchild->child_slot for each entry was initialized when the array of
	 * slots was allocated.
	 */

	dlist_push_head(&ActiveChildList, &pmchild->elem);

	ReservePostmasterChildSlot(pmchild->child_slot);

	elog(LOG, "assigned pm child slot %d for %s", pmchild->child_slot, PostmasterChildName(btype));

	return pmchild;
}

/*
 * Release a PMChild slot, after the child process has exited.
 *
 * Returns true if the child detached cleanly from shared memory, false
 * otherwise (see ReleasePostmasterChildSlot).
 */
bool
FreePostmasterChildSlot(PMChild *pmchild)
{
	elog(LOG, "releasing pm child slot %d for %s", pmchild->child_slot, PostmasterChildName(pmchild->bkend_type));

	dlist_delete(&pmchild->elem);
	if (pmchild->bkend_type == B_DEAD_END_BACKEND)
	{
		pfree(pmchild);
		return true;
	}
	else
	{
		dlist_head *freelist;

		freelist = GetFreeList(pmchild->bkend_type);
		dlist_push_head(freelist, &pmchild->elem);
		return ReleasePostmasterChildSlot(pmchild->child_slot);
	}
}

PMChild *
FindPostmasterChildByPid(pid_or_threadid pid)
{
	dlist_iter	iter;

	dlist_foreach(iter, &ActiveChildList)
	{
		PMChild    *bp = dlist_container(PMChild, elem, iter.cur);

		if (pid_eq(bp->pid, pid))
			return bp;
	}
	return NULL;
}

/*
 * Allocate a PMChild struct for a dead-end backend.  Dead-end children are
 * not assigned a child_slot number.  The struct is palloc'd; returns NULL if
 * out of memory.
 */
PMChild *
AllocDeadEndChild(void)
{
	PMChild    *pmchild;

	elog(LOG, "allocating dead-end child");

	pmchild = (PMChild *) palloc_extended(sizeof(PMChild), MCXT_ALLOC_NO_OOM);
	if (pmchild)
	{
		pmchild->pid.pid = 0;
		pmchild->child_slot = 0;
		pmchild->bkend_type = B_DEAD_END_BACKEND;
		pmchild->rw = NULL;
		pmchild->bgworker_notify = false;

		dlist_push_head(&ActiveChildList, &pmchild->elem);
	}

	return pmchild;
}
