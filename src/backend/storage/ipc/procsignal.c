/*-------------------------------------------------------------------------
 *
 * procsignal.c
 *	  Routines for interprocess signaling
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/procsignal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/parallel.h"
#include "commands/async.h"
#include "ipc/interrupt.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "replication/logicalworker.h"
#include "replication/walsender.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"

/*
 * State for the ProcSignalBarrier mechanism and query cancellation.
 *
 * Each process that wants to participate in barriers or query cancellation
 * registers its process ID in the ProcSignalSlots array. The array is indexed
 * by ProcNumber to make slot allocation simple.
 *
 * The fields in each slot are protected by a spinlock, pss_mutex. pss_pid can
 * also be read without holding the spinlock, as a quick preliminary check
 * when searching for a particular PID in the array.
 *
 * Plain interrupts are intended to be set in cases where we don't need to
 * keep track of whether or not the target process has handled the signal, but
 * sometimes we need confirmation, as when making a global state change that
 * cannot be considered complete until all backends have taken notice of
 * it. For such use cases, we set a bit in pss_barrierCheckMask and then
 * increment the current "barrier generation"; when the new barrier generation
 * (or greater) appears in the pss_barrierGeneration flag of every process,
 * we know that the message has been received everywhere.
 */
typedef struct
{
	pg_atomic_uint32 pss_pid;
	int			pss_cancel_key_len; /* 0 means no cancellation is possible */
	uint8		pss_cancel_key[MAX_CANCEL_KEY_LENGTH];
	slock_t		pss_mutex;		/* protects the above fields */

	/* Barrier-related fields (not protected by pss_mutex) */
	pg_atomic_uint64 pss_barrierGeneration;
	pg_atomic_uint32 pss_barrierCheckMask;
	ConditionVariable pss_barrierCV;
} ProcSignalSlot;

/*
 * Information that is global to the entire ProcSignal system can be stored
 * here.
 *
 * psh_barrierGeneration is the highest barrier generation in existence.
 */
struct ProcSignalHeader
{
	pg_atomic_uint64 psh_barrierGeneration;
	ProcSignalSlot psh_slot[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * We reserve a slot for each possible ProcNumber, plus one for each
 * possible auxiliary process type.  (This scheme assumes there is not
 * more than one of any auxiliary process type at a time, except for
 * IO workers.)
 */
#define NumProcSignalSlots	(MaxBackends + NUM_AUXILIARY_PROCS)

/* Check whether the relevant type bit is set in the flags. */
#define BARRIER_SHOULD_CHECK(flags, type) \
	(((flags) & (((uint32) 1) << (uint32) (type))) != 0)

/* Clear the relevant type bit from the flags. */
#define BARRIER_CLEAR_BIT(flags, type) \
	((flags) &= ~(((uint32) 1) << (uint32) (type)))

NON_EXEC_STATIC ProcSignalHeader *ProcSignal = NULL;
static ProcSignalSlot *MyProcSignalSlot = NULL;

static void CleanupProcSignalState(int status, Datum arg);
static void ResetProcSignalBarrierBits(uint32 flags);

/*
 * ProcSignalShmemSize
 *		Compute space needed for ProcSignal's shared memory
 */
Size
ProcSignalShmemSize(void)
{
	Size		size;

	size = mul_size(NumProcSignalSlots, sizeof(ProcSignalSlot));
	size = add_size(size, offsetof(ProcSignalHeader, psh_slot));
	return size;
}

/*
 * ProcSignalShmemInit
 *		Allocate and initialize ProcSignal's shared memory
 */
void
ProcSignalShmemInit(void)
{
	Size		size = ProcSignalShmemSize();
	bool		found;

	ProcSignal = (ProcSignalHeader *)
		ShmemInitStruct("ProcSignal", size, &found);

	/* If we're first, initialize. */
	if (!found)
	{
		int			i;

		pg_atomic_init_u64(&ProcSignal->psh_barrierGeneration, 0);

		for (i = 0; i < NumProcSignalSlots; ++i)
		{
			ProcSignalSlot *slot = &ProcSignal->psh_slot[i];

			SpinLockInit(&slot->pss_mutex);
			pg_atomic_init_u32(&slot->pss_pid, 0);
			slot->pss_cancel_key_len = 0;
			pg_atomic_init_u64(&slot->pss_barrierGeneration, PG_UINT64_MAX);
			pg_atomic_init_u32(&slot->pss_barrierCheckMask, 0);
			ConditionVariableInit(&slot->pss_barrierCV);
		}
	}
}

/*
 * ProcSignalInit
 *		Register the current process in the ProcSignal array
 */
void
ProcSignalInit(const uint8 *cancel_key, int cancel_key_len)
{
	ProcSignalSlot *slot;
	uint64		barrier_generation;
	uint32		old_pss_pid;

	Assert(cancel_key_len >= 0 && cancel_key_len <= MAX_CANCEL_KEY_LENGTH);
	if (MyProcNumber < 0)
		elog(ERROR, "MyProcNumber not set");
	if (MyProcNumber >= NumProcSignalSlots)
		elog(ERROR, "unexpected MyProcNumber %d in ProcSignalInit (max %d)", MyProcNumber, NumProcSignalSlots);
	slot = &ProcSignal->psh_slot[MyProcNumber];

	SpinLockAcquire(&slot->pss_mutex);

	/* Value used for sanity check below */
	old_pss_pid = pg_atomic_read_u32(&slot->pss_pid);

	/*
	 * Initialize barrier state. Since we're a brand-new process, there
	 * shouldn't be any leftover backend-private state that needs to be
	 * updated. Therefore, we can broadcast the latest barrier generation and
	 * disregard any previously-set check bits.
	 *
	 * NB: This only works if this initialization happens early enough in the
	 * startup sequence that we haven't yet cached any state that might need
	 * to be invalidated. That's also why we have a memory barrier here, to be
	 * sure that any later reads of memory happen strictly after this.
	 */
	pg_atomic_write_u32(&slot->pss_barrierCheckMask, 0);
	barrier_generation =
		pg_atomic_read_u64(&ProcSignal->psh_barrierGeneration);
	pg_atomic_write_u64(&slot->pss_barrierGeneration, barrier_generation);

	if (cancel_key_len > 0)
		memcpy(slot->pss_cancel_key, cancel_key, cancel_key_len);
	slot->pss_cancel_key_len = cancel_key_len;
	pg_atomic_write_u32(&slot->pss_pid, MyProcPid);

	SpinLockRelease(&slot->pss_mutex);

	/* Spinlock is released, do the check */
	if (old_pss_pid != 0)
		elog(LOG, "process %d taking over ProcSignal slot %d, but it's not empty",
			 MyProcPid, MyProcNumber);

	/* Remember slot location for CheckProcSignal */
	MyProcSignalSlot = slot;

	/* Set up to release the slot on process exit */
	on_shmem_exit(CleanupProcSignalState, (Datum) 0);
}

/*
 * CleanupProcSignalState
 *		Remove current process from ProcSignal mechanism
 *
 * This function is called via on_shmem_exit() during backend shutdown.
 */
static void
CleanupProcSignalState(int status, Datum arg)
{
	pid_t		old_pid;
	ProcSignalSlot *slot = MyProcSignalSlot;

	/*
	 * Clear MyProcSignalSlot, so that if INTERRUPT_BARRIER is received after
	 * this point, ProcessProcSignalBarrier() won't try to access it after
	 * it's no longer ours (and perhaps even after we've unmapped the shared
	 * memory segment).
	 */
	Assert(MyProcSignalSlot != NULL);
	MyProcSignalSlot = NULL;

	/* sanity check */
	SpinLockAcquire(&slot->pss_mutex);
	old_pid = pg_atomic_read_u32(&slot->pss_pid);
	if (old_pid != MyProcPid)
	{
		/*
		 * don't ERROR here. We're exiting anyway, and don't want to get into
		 * infinite loop trying to exit
		 */
		SpinLockRelease(&slot->pss_mutex);
		elog(LOG, "process %d releasing ProcSignal slot %d, but it contains %d",
			 MyProcPid, (int) (slot - ProcSignal->psh_slot), (int) old_pid);
		return;					/* XXX better to zero the slot anyway? */
	}

	/* Mark the slot as unused */
	pg_atomic_write_u32(&slot->pss_pid, 0);
	slot->pss_cancel_key_len = 0;

	/*
	 * Make this slot look like it's absorbed all possible barriers, so that
	 * no barrier waits block on it.
	 */
	pg_atomic_write_u64(&slot->pss_barrierGeneration, PG_UINT64_MAX);

	SpinLockRelease(&slot->pss_mutex);

	ConditionVariableBroadcast(&slot->pss_barrierCV);
}

/*
 * EmitProcSignalBarrier
 *		Send a signal to every Postgres process
 *
 * The return value of this function is the barrier "generation" created
 * by this operation. This value can be passed to WaitForProcSignalBarrier
 * to wait until it is known that every participant in the ProcSignal
 * mechanism has absorbed the signal (or started afterwards).
 *
 * Note that it would be a bad idea to use this for anything that happens
 * frequently, as interrupting every backend could cause a noticeable
 * performance hit.
 *
 * Callers are entitled to assume that this function will not throw ERROR
 * or FATAL.
 */
uint64
EmitProcSignalBarrier(ProcSignalBarrierType type)
{
	uint32		flagbit = 1 << (uint32) type;
	uint64		generation;

	/*
	 * Set all the flags.
	 *
	 * Note that pg_atomic_fetch_or_u32 has full barrier semantics, so this is
	 * totally ordered with respect to anything the caller did before, and
	 * anything that we do afterwards. (This is also true of the later call to
	 * pg_atomic_add_fetch_u64.)
	 */
	for (int i = 0; i < NumProcSignalSlots; i++)
	{
		volatile ProcSignalSlot *slot = &ProcSignal->psh_slot[i];

		pg_atomic_fetch_or_u32(&slot->pss_barrierCheckMask, flagbit);
	}

	/*
	 * Increment the generation counter.
	 */
	generation =
		pg_atomic_add_fetch_u64(&ProcSignal->psh_barrierGeneration, 1);

	/*
	 * Send an interrupt to all the processes, so that they update their
	 * advertised barrier generation.
	 *
	 * Concurrency is not a problem here. Backends that have exited don't
	 * matter, and new backends that have joined since we entered this
	 * function must already have current state, since the caller is
	 * responsible for making sure that the relevant state is entirely visible
	 * before calling this function in the first place. We still have to wake
	 * them up - because we can't distinguish between such backends and older
	 * backends that need to update state - but they won't actually need to
	 * change any state.
	 */
	for (ProcNumber pgprocno = 0; pgprocno < NumProcSignalSlots; pgprocno++)
	{
		if (pgprocno == MyProcNumber)
			RaiseInterrupt(INTERRUPT_BARRIER);
		else
			SendInterrupt(INTERRUPT_BARRIER, pgprocno);
	}

	return generation;
}

/*
 * WaitForProcSignalBarrier - wait until it is guaranteed that all changes
 * requested by a specific call to EmitProcSignalBarrier() have taken effect.
 */
void
WaitForProcSignalBarrier(uint64 generation)
{
	Assert(generation <= pg_atomic_read_u64(&ProcSignal->psh_barrierGeneration));

	elog(DEBUG1,
		 "waiting for all backends to process ProcSignalBarrier generation "
		 UINT64_FORMAT,
		 generation);

	for (int i = NumProcSignalSlots - 1; i >= 0; i--)
	{
		ProcSignalSlot *slot = &ProcSignal->psh_slot[i];
		uint64		oldval;

		/*
		 * It's important that we check only pss_barrierGeneration here and
		 * not pss_barrierCheckMask. Bits in pss_barrierCheckMask get cleared
		 * before the barrier is actually absorbed, but pss_barrierGeneration
		 * is updated only afterward.
		 */
		oldval = pg_atomic_read_u64(&slot->pss_barrierGeneration);
		while (oldval < generation)
		{
			if (ConditionVariableTimedSleep(&slot->pss_barrierCV,
											5000,
											WAIT_EVENT_PROC_SIGNAL_BARRIER))
				ereport(LOG,
						(errmsg("still waiting for backend with PID %d to accept ProcSignalBarrier",
								(int) pg_atomic_read_u32(&slot->pss_pid))));
			oldval = pg_atomic_read_u64(&slot->pss_barrierGeneration);
		}
		ConditionVariableCancelSleep();
	}

	elog(DEBUG1,
		 "finished waiting for all backends to process ProcSignalBarrier generation "
		 UINT64_FORMAT,
		 generation);

	/*
	 * The caller is probably calling this function because it wants to read
	 * the shared state or perform further writes to shared state once all
	 * backends are known to have absorbed the barrier. However, the read of
	 * pss_barrierGeneration was performed unlocked; insert a memory barrier
	 * to separate it from whatever follows.
	 */
	pg_memory_barrier();
}

/*
 * Perform global barrier related interrupt checking.
 *
 * Any backend that participates in ProcSignal signaling must arrange to
 * call this function periodically. It is called from CHECK_FOR_INTERRUPTS(),
 * which is enough for normal backends, but not necessarily for all types of
 * background processes.
 */
void
ProcessProcSignalBarrier(void)
{
	uint64		local_gen;
	uint64		shared_gen;
	volatile uint32 flags;

	if (MyProcSignalSlot == NULL)
		return;

	/*
	 * It's not unlikely to process multiple barriers at once, before the
	 * signals for all the barriers have arrived. To avoid unnecessary work in
	 * response to subsequent signals, exit early if we already have processed
	 * all of them.
	 */
	local_gen = pg_atomic_read_u64(&MyProcSignalSlot->pss_barrierGeneration);
	shared_gen = pg_atomic_read_u64(&ProcSignal->psh_barrierGeneration);

	Assert(local_gen <= shared_gen);

	if (local_gen == shared_gen)
		return;

	/*
	 * Get and clear the flags that are set for this backend. Note that
	 * pg_atomic_exchange_u32 is a full barrier, so we're guaranteed that the
	 * read of the barrier generation above happens before we atomically
	 * extract the flags, and that any subsequent state changes happen
	 * afterward.
	 *
	 * NB: In order to avoid race conditions, we must zero
	 * pss_barrierCheckMask first and only afterwards try to do barrier
	 * processing. If we did it in the other order, someone could send us
	 * another barrier of some type right after we called the
	 * barrier-processing function but before we cleared the bit. We would
	 * have no way of knowing that the bit needs to stay set in that case, so
	 * the need to call the barrier-processing function again would just get
	 * forgotten. So instead, we tentatively clear all the bits and then put
	 * back any for which we don't manage to successfully absorb the barrier.
	 */
	flags = pg_atomic_exchange_u32(&MyProcSignalSlot->pss_barrierCheckMask, 0);

	/*
	 * If there are no flags set, then we can skip doing any real work.
	 * Otherwise, establish a PG_TRY block, so that we don't lose track of
	 * which types of barrier processing are needed if an ERROR occurs.
	 */
	if (flags != 0)
	{
		bool		success = true;

		PG_TRY();
		{
			/*
			 * Process each type of barrier. The barrier-processing functions
			 * should normally return true, but may return false if the
			 * barrier can't be absorbed at the current time. This should be
			 * rare, because it's pretty expensive.  Every single
			 * CHECK_FOR_INTERRUPTS() will return here until we manage to
			 * absorb the barrier, and that cost will add up in a hurry.
			 *
			 * NB: It ought to be OK to call the barrier-processing functions
			 * unconditionally, but it's more efficient to call only the ones
			 * that might need us to do something based on the flags.
			 */
			while (flags != 0)
			{
				ProcSignalBarrierType type;
				bool		processed = true;

				type = (ProcSignalBarrierType) pg_rightmost_one_pos32(flags);
				switch (type)
				{
					case PROCSIGNAL_BARRIER_SMGRRELEASE:
						processed = ProcessBarrierSmgrRelease();
						break;
					case PROCSIGNAL_BARRIER_UPDATE_XLOG_LOGICAL_INFO:
						processed = ProcessBarrierUpdateXLogLogicalInfo();
						break;
				}

				/*
				 * To avoid an infinite loop, we must always unset the bit in
				 * flags.
				 */
				BARRIER_CLEAR_BIT(flags, type);

				/*
				 * If we failed to process the barrier, reset the shared bit
				 * so we try again later, and set a flag so that we don't bump
				 * our generation.
				 */
				if (!processed)
				{
					ResetProcSignalBarrierBits(((uint32) 1) << type);
					success = false;
				}
			}
		}
		PG_CATCH();
		{
			/*
			 * If an ERROR occurred, we'll need to try again later to handle
			 * that barrier type and any others that haven't been handled yet
			 * or weren't successfully absorbed.
			 */
			ResetProcSignalBarrierBits(flags);
			PG_RE_THROW();
		}
		PG_END_TRY();

		/*
		 * If some barrier types were not successfully absorbed, we will have
		 * to try again later.
		 */
		if (!success)
			return;
	}

	/*
	 * State changes related to all types of barriers that might have been
	 * emitted have now been handled, so we can update our notion of the
	 * generation to the one we observed before beginning the updates. If
	 * things have changed further, it'll get fixed up when this function is
	 * next called.
	 */
	pg_atomic_write_u64(&MyProcSignalSlot->pss_barrierGeneration, shared_gen);
	ConditionVariableBroadcast(&MyProcSignalSlot->pss_barrierCV);
}

/*
 * If it turns out that we couldn't absorb one or more barrier types, either
 * because the barrier-processing functions returned false or due to an error,
 * arrange for processing to be retried later.
 */
static void
ResetProcSignalBarrierBits(uint32 flags)
{
	pg_atomic_fetch_or_u32(&MyProcSignalSlot->pss_barrierCheckMask, flags);
	RaiseInterrupt(INTERRUPT_BARRIER);
}

/*
 * Send a query cancellation signal to backend.
 *
 * Note: This is called from a backend process before authentication.  We
 * cannot take LWLocks yet, but that's OK; we rely on atomic reads of the
 * fields in the ProcSignal slots.
 */
void
SendCancelRequest(int backendPID, const uint8 *cancel_key, int cancel_key_len)
{
	if (backendPID == 0)
	{
		ereport(LOG, (errmsg("invalid cancel request with PID 0")));
		return;
	}

	/*
	 * See if we have a matching backend. Reading the pss_pid and
	 * pss_cancel_key fields is racy, a backend might die and remove itself
	 * from the array at any time.  The probability of the cancellation key
	 * matching wrong process is miniscule, however, so we can live with that.
	 * PIDs are reused too, so sending the signal based on PID is inherently
	 * racy anyway, although OS's avoid reusing PIDs too soon.
	 */
	for (int i = 0; i < NumProcSignalSlots; i++)
	{
		ProcSignalSlot *slot = &ProcSignal->psh_slot[i];
		bool		match;

		if (pg_atomic_read_u32(&slot->pss_pid) != backendPID)
			continue;

		/* Acquire the spinlock and re-check */
		SpinLockAcquire(&slot->pss_mutex);
		if (pg_atomic_read_u32(&slot->pss_pid) != backendPID)
		{
			SpinLockRelease(&slot->pss_mutex);
			continue;
		}
		else
		{
			match = slot->pss_cancel_key_len == cancel_key_len &&
				timingsafe_bcmp(slot->pss_cancel_key, cancel_key, cancel_key_len) == 0;

			SpinLockRelease(&slot->pss_mutex);

			if (match)
			{
				/* Found a match; signal that backend to cancel current op */
				ereport(DEBUG2,
						(errmsg_internal("processing cancel request: sending SIGINT to process %d",
										 backendPID)));

				/*
				 * If we have setsid(), signal the backend's whole process
				 * group
				 *
				 * FIXME: could we send INTERRUPT_QUERY_CANCEL here directly?
				 * We don't have a PGPROC entry yet, but the target backend
				 * presumably does.
				 */
#ifdef HAVE_SETSID
				kill(-backendPID, SIGINT);
#else
				kill(backendPID, SIGINT);
#endif
			}
			else
			{
				/* Right PID, wrong key: no way, Jose */
				ereport(LOG,
						(errmsg("wrong key in cancel request for process %d",
								backendPID)));
			}
			return;
		}
	}

	/* No matching backend */
	ereport(LOG,
			(errmsg("PID %d in cancel request did not match any process",
					backendPID)));
}
