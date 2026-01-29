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

/*
 * Currently installed interrupt handlers
 */
static pg_interrupt_handler_t interrupt_handlers[32];

/*
 * XXX: is 'volatile' still needed on all the variables below? Which ones are
 * accessed from signal handlers?
 */

/* Bitmask of currently enabled interrupts */
volatile InterruptMask EnabledInterruptsMask;

/*
 * Interrupts that would be processed by CHECK_FOR_INTERRUPTS().  This is
 * equal to EnabledInterruptsMask, except when interrupts are held off by
 * HOLD/RESUME_INTERRUPTS() or a critical section.
 */
volatile InterruptMask CheckForInterruptsMask;

/* Variables for holdoff mechanism */
volatile uint32 InterruptHoldoffCount = 0;
volatile uint32 CritSectionCount = 0;

/* A common WaitEventSet used to implement WaitInterrupt() */
static WaitEventSet *InterruptWaitSet;

/* The position of the interrupt in InterruptWaitSet. */
#define InterruptWaitSetInterruptPos 0
#define InterruptWaitSetPostmasterDeathPos 1

static pg_atomic_uint32 LocalPendingInterrupts;

pg_atomic_uint32 *MyPendingInterrupts = &LocalPendingInterrupts;

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
	 * XXX: It's somewhat inefficient to loop through all bits, but this isn't
	 * performance critical.
	 */
	for (uint32 i = 0; i < lengthof(interrupt_handlers); i++)
	{
		if ((interruptMask & (1 << i)) != 0)
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
	for (uint32 i = 0; i < lengthof(interrupt_handlers); i++)
	{
		if ((interruptMask & (1 << i)) != 0)
			Assert(interrupt_handlers[i] != NULL);
	}
#endif

	EnabledInterruptsMask |= interruptMask;
	if (InterruptHoldoffCount == 0 && CritSectionCount == 0)
		CheckForInterruptsMask = EnabledInterruptsMask;
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
	if (InterruptHoldoffCount == 0 && CritSectionCount == 0)
		CheckForInterruptsMask = EnabledInterruptsMask;
}

/*
 * ProcessInterrupts: out-of-line portion of CHECK_FOR_INTERRUPTS() macro
 *
 * If an interrupt condition is pending, and it's safe to service it,
 * then clear the flag and call the interrupt handler.
 *
 * Note: if INTERRUPTS_CAN_BE_PROCESSED(interrupt) is true, then
 * ProcessInterrupts is guaranteed to clear the given interrupt before
 * returning.  (This is not the same as guaranteeing that it's still clear
 * when we return; another interrupt could have arrived.  But we promise that
 * any pre-existing one will have been serviced.)
 */
void
ProcessInterrupts(void)
{
	InterruptMask interruptsToProcess;

	Assert(InterruptHoldoffCount == 0 && CritSectionCount == 0);

	/*
	 * TODO: it'd be good for performance to read the atomic
	 * MyPendingInterrupts variable just once in this function
	 */
	interruptsToProcess =
		pg_atomic_read_u32(MyPendingInterrupts) & CheckForInterruptsMask;

	for (uint32 i = 0; i < lengthof(interrupt_handlers); i++)
	{
		uint32		interrupt = (1 << i);

		if ((interruptsToProcess & interrupt) != 0)
		{
			/*
			 * Clear the interrupt *before* calling the handler function, so
			 * that if the interrupt is received again while the handler
			 * function is being executed, we won't miss it.
			 *
			 * For similar reasons, we also clear the flags one by one even if
			 * multiple interrupts are pending.  Otherwise if one of the
			 * interrupt handlers bail out with an ERROR, we would have
			 * already cleared the other bits, and would miss processing them.
			 */
			ClearInterrupt(interrupt);

			/* Call the handler function */
			(*interrupt_handlers[i]) ();
		}
	}
}

/*
 * Switch to local interrupts.  Other backends can't send interrupts to this
 * one.  Only RaiseInterrupt() can set them, from inside this process.
 */
void
SwitchToLocalInterrupts(void)
{
	if (MyPendingInterrupts == &LocalPendingInterrupts)
		return;

	MyPendingInterrupts = &LocalPendingInterrupts;

	/*
	 * Make sure that SIGALRM handlers that call RaiseInterrupt() are now
	 * seeing the new MyPendingInterrupts destination.
	 */
	pg_memory_barrier();

	/*
	 * Mix in the interrupts that we have received already in our shared
	 * interrupt vector, while atomically clearing it.  Other backends may
	 * continue to set bits in it after this point, but we've atomically
	 * transferred the existing bits to our local vector so we won't get
	 * duplicated interrupts later if we switch back.
	 */
	pg_atomic_fetch_or_u32(MyPendingInterrupts,
						   pg_atomic_exchange_u32(&MyProc->pendingInterrupts, 0));
}

/*
 * Switch to shared memory interrupts.  Other backends can send interrupts to
 * this one if they know its ProcNumber, and we'll now see any that we missed.
 */
void
SwitchToSharedInterrupts(void)
{
	if (MyPendingInterrupts == &MyProc->pendingInterrupts)
		return;

	MyPendingInterrupts = &MyProc->pendingInterrupts;

	/*
	 * Make sure that SIGALRM handlers that call RaiseInterrupt() are now
	 * seeing the new MyPendingInterrupts destination.
	 */
	pg_memory_barrier();

	/* Mix in any unhandled bits from LocalPendingInterrupts. */
	pg_atomic_fetch_or_u32(MyPendingInterrupts,
						   pg_atomic_exchange_u32(&LocalPendingInterrupts, 0));
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
	uint32		old_pending;

	old_pending = pg_atomic_fetch_or_u32(MyPendingInterrupts, interruptMask);

	/*
	 * If the process is currently blocked waiting for an interrupt to arrive,
	 * and the interrupt wasn't already pending, wake it up.
	 */
	if ((old_pending & (interruptMask | SLEEPING_ON_INTERRUPTS)) == SLEEPING_ON_INTERRUPTS)
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
	uint32		old_pending;

	Assert(pgprocno != INVALID_PROC_NUMBER);
	Assert(pgprocno >= 0);
	Assert(pgprocno < ProcGlobal->allProcCount);

	proc = &ProcGlobal->allProcs[pgprocno];
	old_pending = pg_atomic_fetch_or_u32(&proc->pendingInterrupts, interruptMask);

	elog(DEBUG1, "sending interrupt %08x to pid %d", interruptMask, proc->pid);

	/*
	 * If the process is currently blocked waiting for an interrupt to arrive,
	 * and the interrupt wasn't already pending, wake it up.
	 */
	if ((old_pending & (interruptMask | SLEEPING_ON_INTERRUPTS)) == SLEEPING_ON_INTERRUPTS)
		WakeupOtherProc(proc);
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
 * This is used as the INTERRUPT_TERMINATE handler in aux processes that want
 * to just exit immediately.
 */
void
ProcessAuxProcessShutdownInterrupt(void)
{
	proc_exit(0);
}
