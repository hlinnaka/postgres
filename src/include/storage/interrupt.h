/*-------------------------------------------------------------------------
 *
 * interrupt.h
 *	  Inter-process interrupts
 *
 * "Interrupts" are a set of flags that represent conditions that should be
 * handled at a later time.  They are roughly analogous to Unix signals,
 * except that they are handled cooperatively by checking for them at many
 * points in the code.
 *
 * Interrupt flags can be "raised" synchronously by code that wants to defer
 * an action, or asynchronously by timer signal handlers, other signal
 * handlers or "sent" by other backends setting them directly.
 *
 * Most code currently deals with the INTERRUPT_GENERAL interrupt. It is
 * raised by any of the events checked by CHECK_FOR_INTERRUPTS(), like query
 * cancellation or idle session timeout. Well behaved backend code performs
 * CHECK_FOR_INTERRUPTS() periodically in long computations, and should never
 * sleep using mechanisms other than the WaitEventSet mechanism or the more
 * convenient WaitInterrupt/WaitSockerOrInterrupt functions (except for
 * bounded short periods, eg LWLock waits), so they should react in good time.
 *
 * The "standard" set of interrupts is handled by CHECK_FOR_INTERRUPTS(), and
 * consists of tasks that are safe to perform at most times.  They can be
 * suppressed by HOLD_INTERRUPTS()/RESUME_INTERRUPTS().
 *
 *
 * The correct pattern to wait for event(s) using INTERRUPT_GENERAL is:
 *
 * for (;;)
 * {
 *	   ClearInterrupt(INTERRUPT_GENERAL);
 *	   if (work to do)
 *		   Do Stuff();
 *	   WaitInterrupt(1 << INTERRUPT_GENERAL, ...);
 * }
 *
 * It's important to clear the interrupt *before* checking if there's work to
 * do. Otherwise, if someone sets the interrupt between the check and the
 * ClearInterrupt() call, you will miss it and Wait will incorrectly block.
 *
 * Another valid coding pattern looks like:
 *
 * for (;;)
 * {
 *	   if (work to do)
 *		   Do Stuff(); // in particular, exit loop if some condition satisfied
 *	   WaitInterrupt(1 << INTERRUPT_GENERAL, ...);
 *	   ClearInterrupt(INTERRUPT_GENERAL);
 * }
 *
 * This is useful to reduce interrupt traffic if it's expected that the loop's
 * termination condition will often be satisfied in the first iteration; the
 * cost is an extra loop iteration before blocking when it is not.  What must
 * be avoided is placing any checks for asynchronous events after
 * WaitInterrupt and before ClearInterrupt, as that creates a race condition.
 *
 * To wake up the waiter, you must first set a global flag or something else
 * that the wait loop tests in the "if (work to do)" part, and call
 * SendInterrupt(INTERRUPT_GENERAL) *after* that. SendInterrupt is designed to
 * return quickly if the interrupt is already set. In more complex scenarios
 * with nested loops that can consume different events, you can define your
 * own INTERRUPT_* flag instead of relying on INTERRUPT_GENERAL.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/storage/interrupt.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef STORAGE_INTERRUPT_H
#define STORAGE_INTERRUPT_H

#include "port/atomics.h"
#include "storage/procnumber.h"
#include "storage/waiteventset.h"

#include <signal.h>

extern PGDLLIMPORT pg_atomic_uint32 *MyPendingInterrupts;

/*
 * Flags in the pending interrupts bitmask. Each value represents one bit in
 * the bitmask.
 */
typedef enum
{
	/*
	 * SLEEPING_ON_INTERRUPTS indicates that the backend is currently blocked
	 * waiting for an interrupt. If it's set, the backend needs to be woken up
	 * when a bit in the pending interrupts mask is set. It's used internally
	 * by the interrupt machinery, and cannot be used directly in the public
	 * functions. It's named differently to distinguish it from the actual
	 * interrupt flags.
	 */
	SLEEPING_ON_INTERRUPTS = 0,

	/*
	 * INTERRUPT_GENERAL is multiplexed for many reasons, like query
	 * cancellation termination requests, recovery conflicts, and config
	 * reload requests.  Upon receiving INTERRUPT_GENERAL, you should call
	 * CHECK_FOR_INTERRUPTS() to process those requests.  It is also used for
	 * various other context-dependent purposes, but note that if it's used to
	 * wake up for other reasons, you must still call CHECK_FOR_INTERRUPTS()
	 * once per iteration.
	 */
	INTERRUPT_GENERAL,

	/*
	 * INTERRUPT_RECOVERY_WAKEUP is used to wake up startup process, to tell
	 * it that it should continue WAL replay. It's sent by WAL receiver when
	 * more WAL arrives, or when promotion is requested.
	 */
	INTERRUPT_RECOVERY_CONTINUE,
} InterruptType;

/*
 * Test an interrupt flag.
 */
static inline bool
InterruptIsPending(InterruptType reason)
{
	return (pg_atomic_read_u32(MyPendingInterrupts) & (1 << reason)) != 0;
}

/*
 * Test an interrupt flag.
 */
static inline bool
InterruptsPending(uint32 mask)
{
	return (pg_atomic_read_u32(MyPendingInterrupts) & (mask)) != 0;
}

/*
 * Clear an interrupt flag.
 */
static inline void
ClearInterrupt(InterruptType reason)
{
	pg_atomic_fetch_and_u32(MyPendingInterrupts, ~(1 << reason));
}

/*
 * Test and clear an interrupt flag.
 */
static inline bool
ConsumeInterrupt(InterruptType reason)
{
	if (likely(!InterruptIsPending(reason)))
		return false;

	ClearInterrupt(reason);

	return true;
}

extern void RaiseInterrupt(InterruptType reason);
extern void SendInterrupt(InterruptType reason, ProcNumber pgprocno);
extern int	WaitInterrupt(uint32 interruptMask, int wakeEvents, long timeout,
						  uint32 wait_event_info);
extern int	WaitInterruptOrSocket(uint32 interruptMask, int wakeEvents, pgsocket sock,
								  long timeout, uint32 wait_event_info);
extern void SwitchToLocalInterrupts(void);
extern void SwitchToSharedInterrupts(void);
extern void InitializeInterruptWaitSet(void);

#endif
