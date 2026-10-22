/*-------------------------------------------------------------------------
 *
 * interrupt.c
 *	  Inter-process interrupts.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/ipc/interrupt.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "ipc/interrupt.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "utils/resowner.h"


/* Variables for the holdoff mechanism */
uint32		InterruptHoldoffCount = 0;
uint32		CritSectionCount = 0;

/*
 * Currently installed interrupt handlers
 */
static pg_interrupt_handler_t interrupt_handlers[64];

/* Bitmask of currently enabled interrupts */
InterruptMask EnabledInterruptsMask;

/* A common WaitEventSet used to implement WaitInterrupt() */
static WaitEventSet *InterruptWaitSet;

/* The position of the interrupt in InterruptWaitSet. */
#define InterruptWaitSetInterruptPos 0
#define InterruptWaitSetPostmasterDeathPos 1

static PendingInterrupts LocalPendingInterrupts;
PendingInterrupts *MyPendingInterrupts = &LocalPendingInterrupts;
pg_atomic_uint32 *MyPendingInterruptsFlags = &LocalPendingInterrupts.flags;
const pg_atomic_uint32 ZeroPendingInterruptsFlags;

static int	nextAddinInterruptBit = BEGIN_ADDIN_INTERRUPTS;

/*
 * Install an interrupt handler callback function for the given interrupt.
 *
 * You need to also enable the interrupt with EnableInterrupt(), unless you're
 * replacing an existing handler function.
 */
void
SetInterruptHandler(InterruptMask interruptMask, pg_interrupt_handler_t handler)
{
	/*
	 * XXX: It's somewhat inefficient to loop through all the bits, but this
	 * isn't performance critical.
	 */
	for (int i = 0; i < lengthof(interrupt_handlers); i++)
	{
		if ((interruptMask & UINT64_BIT(i)) != 0)
		{
			/* Replace old handler */
			interrupt_handlers[i] = handler;
		}
	}
}

/* Enable an interrupt to be processed by CHECK_FOR_INTERRUPTS() */
void
EnableInterrupt(InterruptMask interruptMask)
{
#ifdef USE_ASSERT_CHECKING
	/* Check that the interrupt has a handler defined */
	for (int i = 0; i < lengthof(interrupt_handlers); i++)
	{
		if ((interruptMask & UINT64_BIT(i)) != 0)
			Assert(interrupt_handlers[i] != NULL);
	}
#endif
	EnabledInterruptsMask |= interruptMask;
	SetInterruptAttentionMask(EnabledInterruptsMask);
}

/*
 * Disable the handler function for an interrupt.
 *
 * When disabled, CHECK_FOR_INTERRUPTS() will not call the handler function
 * for the given interrupt.  If the interrupt is received, it will remain
 * pending until you manually check and clear it with ClearInterrupt(), or
 * re-enable the handler function.
 */
void
DisableInterrupt(InterruptMask interruptMask)
{
	EnabledInterruptsMask &= ~interruptMask;
#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	pg_atomic_write_u64(&MyPendingInterrupts->attention_mask, EnabledInterruptsMask);
#else
	pg_atomic_write_u32(&MyPendingInterrupts->attention_mask_lo, (uint32) EnabledInterruptsMask);
	pg_atomic_write_u32(&MyPendingInterrupts->attention_mask_hi, (uint32) (EnabledInterruptsMask >> 32));
#endif

	/*
	 * Note: the ATTENTION flag might now be unnecessarily set. We don't try
	 * to clear it here, the next CHECK_FOR_INTERRUPTS() will take care of it.
	 */
}

/*
 * Reset InterruptHoldoffCount and CritSectionCount to given values.  Used
 * when recovering from an error.
 */
void
ResetInterruptHoldoffCounts(uint32 new_holdoff_count, uint32 new_crit_section_count)
{
	InterruptHoldoffCount = new_holdoff_count;
	CritSectionCount = new_crit_section_count;
	if (InterruptHoldoffCount == 0 && CritSectionCount == 0)
		MyPendingInterruptsFlags = &MyPendingInterrupts->flags;
	else
		MyPendingInterruptsFlags = unconstify(pg_atomic_uint32 *, &ZeroPendingInterruptsFlags);
}

/*
 * ProcessInterrupts: out-of-line portion of CHECK_FOR_INTERRUPTS() macro
 *
 * If an interrupt condition is pending, and it's safe to service it, then
 * clear the flag and call the interrupt handler.
 *
 * Note: if INTERRUPTS_CAN_BE_PROCESSED() is true, ProcessInterrupts() is
 * guaranteed to clear all the enabled interrupts before returning.  (This is
 * not the same as guaranteeing that it's still clear when we return; another
 * interrupt could have arrived.  But we promise that any pre-existing one
 * will have been serviced.)
 */
void
ProcessInterrupts(void)
{
	uint64		pending;
	InterruptMask interruptsToProcess;

	Assert(INTERRUPTS_CAN_BE_PROCESSED());
	Assert((pg_atomic_read_u32(&MyPendingInterrupts->flags) & PI_FLAG_SLEEPING) == 0);

	/*
	 * Clear the ATTENTION flag first. This ensures that if any interrupts are
	 * received while we're processing, the flag is set again.
	 */
	(void) pg_atomic_write_u32(&MyPendingInterrupts->flags, 0);

	/*
	 * Make sure others see the clearing of the flags, before we read the
	 * pending interrupts.
	 */
	pg_memory_barrier();

	/* Check once what interrupts are pending */
#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	pending = pg_atomic_read_u64(&MyPendingInterrupts->pending_mask);
#else
	pending = (uint64) pg_atomic_read_u32(&MyPendingInterrupts->pending_mask_lo);
	pending |= (uint64) pg_atomic_read_u32(&MyPendingInterrupts->pending_mask_hi) << 32;
#endif
	interruptsToProcess = pending & EnabledInterruptsMask;

	if (interruptsToProcess != 0)
	{
		Assert(InterruptHoldoffCount == 0 && CritSectionCount == 0);

		/* Interrupt handlers are not expected to be re-entrant. */
		HOLD_INTERRUPTS();

		for (int i = 0; i < lengthof(interrupt_handlers); i++)
		{
			if ((interruptsToProcess & UINT64_BIT(i)) != 0)
			{
				/*
				 * Clear the interrupt *before* calling the handler function,
				 * so that if the interrupt is received again while the
				 * handler function is being executed, we won't miss it.
				 *
				 * For similar reasons, we also clear the flags one by one
				 * even if multiple interrupts are pending.  Otherwise if one
				 * of the interrupt handlers bail out with an ERROR, we would
				 * have already cleared the other bits, and would miss
				 * processing them.
				 */
				ClearInterrupt(UINT64_BIT(i));

				/* Call the handler function */
				(*interrupt_handlers[i]) ();
			}
		}

		RESUME_INTERRUPTS();
	}

	/*
	 * If we get here, we processed all the interrupts that were pending when
	 * we started.  If any new interrupts arrived while we were processing,
	 * they must've set the ATTENTION flag again so we'll get back here on the
	 * next CHECK_FOR_INTERRUPTS().
	 */
}

/*
 * Update MyPendingInterrupts->attention_mask, setting PI_FLAG_ATTENTION if
 * any of the interrupts in the new mask are already pending.  This should be
 * called every time after enabling new bits in EnabledInterruptsMask, so that
 * the next CHECK_FOR_INTERRUPTS() will react correctly to the newly enabled
 * interrupts.
 */
void
SetInterruptAttentionMask(InterruptMask mask)
{
	/*
	 * This should not be called while sleeping. No other process sets the
	 * flag, so when we clear/set 'flags' below, we don't need to worry about
	 * overwriting it.
	 */
	Assert((pg_atomic_read_u32(&MyPendingInterrupts->flags) & PI_FLAG_SLEEPING) == 0);

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	pg_atomic_write_u64(&MyPendingInterrupts->attention_mask, mask);
#else
	pg_atomic_write_u32(&MyPendingInterrupts->attention_mask_lo, (uint32) mask);
	pg_atomic_write_u32(&MyPendingInterrupts->attention_mask_hi, (uint32) (mask >> 32));
#endif

	/*
	 * Make sure other processes see the updated attention_mask before we read
	 * the interrupts that are currently pending.
	 *
	 * XXX: It might be cheaper to use pg_atomic_write_membarrier_u64 variant
	 * above, paired with pg_atomic_read_membarrier_u64() here to read the
	 * pending interrupts instead of the barrier-less InterruptPending().
	 */
	pg_memory_barrier();

	if (InterruptPending(mask))
		pg_atomic_write_u32(&MyPendingInterrupts->flags, PI_FLAG_ATTENTION);
}


/*
 * Make 'new_ptr' the active interrupt vector, transfering all the pending
 * interrupt bits from the old MyPendingInterrupts vector to the new one.
 */
static void
SwitchMyPendingInterruptsPtr(PendingInterrupts *new_ptr)
{
	PendingInterrupts *old_ptr = MyPendingInterrupts;

	/* should not be called while sleeping */
	Assert((pg_atomic_read_u32(&MyPendingInterrupts->flags) & PI_FLAG_SLEEPING) == 0);

	if (new_ptr == old_ptr)
		return;

	MyPendingInterrupts = new_ptr;
	if (MyPendingInterruptsFlags == &old_ptr->flags)
		MyPendingInterruptsFlags = &new_ptr->flags;

	/*
	 * Make sure that SIGALRM handlers that call RaiseInterrupt() are now
	 * seeing the new MyPendingInterrupts destination.
	 */
	pg_memory_barrier();

	/*
	 * Mix in the interrupts that we have received already in 'new_ptr', while
	 * atomically clearing them from 'old_ptr'.  Other backends may continue
	 * to set bits in 'old_ptr' after this point, but we've atomically
	 * transferred the existing bits to our local vector so we won't get
	 * duplicated interrupts later if we switch back.
	 */
#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	{
		uint64		old_pending;

		old_pending = pg_atomic_exchange_u64(&old_ptr->pending_mask, 0);
		pg_atomic_fetch_or_u64(&new_ptr->pending_mask, old_pending);
	}
#else
	{
		uint32		old_pending_lo;
		uint32		old_pending_hi;

		old_pending_lo = pg_atomic_exchange_u32(&old_ptr->pending_mask_lo, 0);
		old_pending_hi = pg_atomic_exchange_u32(&old_ptr->pending_mask_hi, 0);
		pg_atomic_fetch_or_u32(&new_ptr->pending_mask_lo, old_pending_lo);
		pg_atomic_fetch_or_u32(&new_ptr->pending_mask_hi, old_pending_hi);
	}
#endif

	SetInterruptAttentionMask(EnabledInterruptsMask);
}

/*
 * Switch to local interrupts.  Other backends can't send interrupts to this
 * one.  Only RaiseInterrupt() can set them, from inside this process.
 */
void
SwitchToLocalInterrupts(void)
{
	SwitchMyPendingInterruptsPtr(&LocalPendingInterrupts);
}

/*
 * Switch to shared memory interrupts.  Other backends can send interrupts to
 * this one if they know its ProcNumber, and we'll now see any that we missed.
 */
void
SwitchToSharedInterrupts(void)
{
	SwitchMyPendingInterruptsPtr(&MyProc->pendingInterrupts);
}

static bool
SendOrRaiseInterrupt(PendingInterrupts *ptr, InterruptMask interruptMask)
{
	uint64		old_pending;
	uint64		attention_mask;
	uint32		old_flags;
	bool		wakeup = false;

	/*
	 * Do an "unlocked" read first, for a quick exit if all the bits are
	 * already set.
	 */
#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	old_pending = pg_atomic_read_u64(&ptr->pending_mask);
#else
	old_pending = (uint64) pg_atomic_read_u32(&ptr->pending_mask_lo);
	old_pending |= (uint64) pg_atomic_read_u32(&ptr->pending_mask_hi) << 32;
#endif

	if ((interruptMask & ~old_pending) == 0)
		return false;			/* no new bits were set */

	/* OR our bits to the target */
#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	(void) pg_atomic_fetch_or_u64(&ptr->pending_mask, interruptMask);
#else
	(void) pg_atomic_fetch_or_u32(&ptr->pending_mask_lo, (uint32) interruptMask);
	(void) pg_atomic_fetch_or_u32(&ptr->pending_mask_hi, (uint32) (interruptMask >> 32));
#endif

	/*
	 * Did we set any bits that the requires the target process's ATTENTION?
	 */
#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	attention_mask = pg_atomic_read_u64(&ptr->attention_mask);
#else
	attention_mask = (uint64) pg_atomic_read_u32(&ptr->attention_mask_lo);
	attention_mask |= (uint64) pg_atomic_read_u32(&ptr->attention_mask_hi) << 32;
#endif
	if ((attention_mask & interruptMask) != 0)
	{
		old_flags = pg_atomic_fetch_or_u32(&ptr->flags, PI_FLAG_ATTENTION);

		/*
		 * Furthermore, if the process is currently sleeping on these
		 * interrupts, wake it up.
		 */
		if ((old_flags & PI_FLAG_SLEEPING) != 0)
			wakeup = true;
	}

	return wakeup;
}

/*
 * Set an interrupt flag in this backend.
 *
 * Note: This is called from signal handlers, so needs to be async-signal
 * safe!
 */
void
RaiseInterrupt(InterruptMask interruptMask)
{
	if (SendOrRaiseInterrupt(MyPendingInterrupts, interruptMask))
		WakeupMyProc();
}

/*
 * Set an interrupt flag in another backend.
 *
 * Note: This can also be called from the postmaster, so be careful to not
 * trust the contents of shared memory.
 *
 * FIXME: it's easy to accidentally swap the order of the args.  Could we have
 * stricter type checking?
 */
void
SendInterrupt(InterruptMask interruptMask, ProcNumber pgprocno)
{
	PGPROC	   *proc;

	Assert(pgprocno != INVALID_PROC_NUMBER);
	Assert(pgprocno >= 0);
	Assert(pgprocno < ProcGlobal->allProcCount);

	proc = &ProcGlobal->allProcs[pgprocno];

	/*
	 * If the process is currently blocked waiting for an interrupt to arrive,
	 * and the interrupt wasn't already pending, wake it up.
	 */
	if (SendOrRaiseInterrupt(&proc->pendingInterrupts, interruptMask))
		WakeupOtherProc(proc);
}

/*
 * Like SendInterrupt, but with a cross-check that the process has the given
 * PID.
 *
 * This acquires ProcArrayLock to ensure atomicity, i.e. that the process
 * doesn't go away while we're about to send the interrupt, so this cannot be
 * used from postmaster.
 *
 * Most interrupts are harmless to send to wrong process, but with others like
 * INTERRUPT_TERMINATE, not so much.
 */
void
SendInterruptWithPid(InterruptMask interruptMask, ProcNumber pgprocno, pid_t pid)
{
	PGPROC	   *proc;

	Assert(pgprocno != INVALID_PROC_NUMBER);
	Assert(pgprocno >= 0);
	Assert(pgprocno < ProcGlobal->allProcCount);

	proc = &ProcGlobal->allProcs[pgprocno];

	LWLockAcquire(ProcArrayLock, LW_SHARED);
	if (proc->pid == pid)
	{

		/*
		 * If the process is currently blocked waiting for an interrupt to
		 * arrive, and the interrupt wasn't already pending, wake it up.
		 */
		if (SendOrRaiseInterrupt(&proc->pendingInterrupts, interruptMask))
			WakeupOtherProc(proc);
	}
	LWLockRelease(ProcArrayLock);
}

void
InitializeInterruptWaitSet(void)
{
	int			interrupt_pos PG_USED_FOR_ASSERTS_ONLY;

	Assert(InterruptWaitSet == NULL);

	/* Set up the WaitEventSet used by WaitInterrupt(). */
	InterruptWaitSet = CreateWaitEventSet(NULL, 2);
	interrupt_pos = AddWaitEventToSet(InterruptWaitSet, WL_INTERRUPT, PGINVALID_SOCKET,
									  0, NULL);
	if (IsUnderPostmaster)
		AddWaitEventToSet(InterruptWaitSet, WL_EXIT_ON_PM_DEATH,
						  PGINVALID_SOCKET, 0, NULL);

	Assert(interrupt_pos == InterruptWaitSetInterruptPos);
}

/*
 * Wait for any of the interrupts in interruptMask to be set, or for
 * postmaster death, or until timeout is exceeded. 'wakeEvents' is a bitmask
 * that specifies which of those events to wait for. If the interrupt is
 * already pending (and WL_INTERRUPT is given), the function returns
 * immediately.
 *
 * The "timeout" is given in milliseconds. It must be >= 0 if WL_TIMEOUT flag
 * is given.  Although it is declared as "long", we don't actually support
 * timeouts longer than INT_MAX milliseconds.  Note that some extra overhead
 * is incurred when WL_TIMEOUT is given, so avoid using a timeout if possible.
 *
 * Returns bit mask indicating which condition(s) caused the wake-up. Note
 * that if multiple wake-up conditions are true, there is no guarantee that
 * we return all of them in one call, but we will return at least one.
 */
int
WaitInterrupt(InterruptMask interruptMask, int wakeEvents, long timeout,
			  uint32 wait_event_info)
{
	WaitEvent	event;

	/* Postmaster-managed callers must handle postmaster death somehow. */
	Assert(!IsUnderPostmaster ||
		   (wakeEvents & WL_EXIT_ON_PM_DEATH) ||
		   (wakeEvents & WL_POSTMASTER_DEATH));

	/*
	 * Some callers may have an interrupt mask different from last time, or no
	 * interrupt mask at all, or want to handle postmaster death differently.
	 * It's cheap to assign those, so just do it every time.
	 */
	if (!(wakeEvents & WL_INTERRUPT))
		interruptMask = 0;
	ModifyWaitEvent(InterruptWaitSet, InterruptWaitSetInterruptPos,
					WL_INTERRUPT, interruptMask);

	ModifyWaitEvent(InterruptWaitSet, InterruptWaitSetPostmasterDeathPos,
					(wakeEvents & (WL_EXIT_ON_PM_DEATH | WL_POSTMASTER_DEATH)),
					0);

	if (WaitEventSetWait(InterruptWaitSet,
						 (wakeEvents & WL_TIMEOUT) ? timeout : -1,
						 &event, 1,
						 wait_event_info) == 0)
		return WL_TIMEOUT;
	else
		return event.events;
}

/*
 * Like WaitInterrupt, but with an extra socket argument for WL_SOCKET_*
 * conditions.
 *
 * When waiting on a socket, EOF and error conditions always cause the socket
 * to be reported as readable/writable/connected, so that the caller can deal
 * with the condition.
 *
 * wakeEvents must include either WL_EXIT_ON_PM_DEATH for automatic exit
 * if the postmaster dies or WL_POSTMASTER_DEATH for a flag set in the
 * return value if the postmaster dies.  The latter is useful for rare cases
 * where some behavior other than immediate exit is needed.
 *
 * NB: These days this is just a wrapper around the WaitEventSet API. When
 * using an interrupt very frequently, consider creating a longer living
 * WaitEventSet instead; that's more efficient.
 */
int
WaitInterruptOrSocket(InterruptMask interruptMask, int wakeEvents, pgsocket sock,
					  long timeout, uint32 wait_event_info)
{
	int			ret;
	int			rc;
	WaitEvent	event;
	WaitEventSet *set = CreateWaitEventSet(CurrentResourceOwner, 3);

	if (wakeEvents & WL_TIMEOUT)
		Assert(timeout >= 0);
	else
		timeout = -1;

	if (wakeEvents & WL_INTERRUPT)
		AddWaitEventToSet(set, WL_INTERRUPT, PGINVALID_SOCKET,
						  interruptMask, NULL);

	/* Postmaster-managed callers must handle postmaster death somehow. */
	Assert(!IsUnderPostmaster ||
		   (wakeEvents & WL_EXIT_ON_PM_DEATH) ||
		   (wakeEvents & WL_POSTMASTER_DEATH));

	if ((wakeEvents & WL_POSTMASTER_DEATH) && IsUnderPostmaster)
		AddWaitEventToSet(set, WL_POSTMASTER_DEATH, PGINVALID_SOCKET,
						  0, NULL);

	if ((wakeEvents & WL_EXIT_ON_PM_DEATH) && IsUnderPostmaster)
		AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
						  0, NULL);

	if (wakeEvents & WL_SOCKET_MASK)
	{
		int			ev;

		ev = wakeEvents & WL_SOCKET_MASK;
		AddWaitEventToSet(set, ev, sock, 0, NULL);
	}

	rc = WaitEventSetWait(set, timeout, &event, 1, wait_event_info);
	if (rc == 0)
		ret = WL_TIMEOUT;
	else
	{
		ret = event.events & (WL_INTERRUPT |
							  WL_POSTMASTER_DEATH |
							  WL_SOCKET_MASK);
	}

	FreeWaitEventSet(set);

	return ret;
}

/*
 * This is used as the INTERRUPT_TERMINATE handler in some aux processes that
 * want to just exit immediately.
 */
void
ProcessAuxProcessShutdownInterrupt(void)
{
	proc_exit(0);
}

/* Reserve an interrupt bit for use in an extension */
InterruptMask
RequestAddinInterrupt(void)
{
	InterruptMask result;

	if (nextAddinInterruptBit == END_ADDIN_INTERRUPTS)
		elog(ERROR, "out of addin interrupt bits");

	result = UINT64_BIT(nextAddinInterruptBit);
	nextAddinInterruptBit++;
	return result;
}
