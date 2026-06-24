/*-------------------------------------------------------------------------
 *
 * interrupt.h
 *	  Inter-process interrupts
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/ipc/interrupt.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef IPC_INTERRUPT_H
#define IPC_INTERRUPT_H

#include <signal.h>

#include "port/atomics.h"
#include "storage/procnumber.h"

/*
 * Include waiteventset.h for the WL_* flags. They're not needed her, but are
 * needed which are needed by all callers of WaitInterrupt, so include it
 * here.
 *
 * Note: InterruptMask is defined in waiteventset.h to avoid circular dependency
 */
#include "storage/waiteventset.h"

/*****************************************************************************
 *	  System interrupt and critical section handling
 *
 * There are two types of interrupts that a running backend needs to accept
 * without messing up its state: QueryCancel (SIGINT) and ProcDie (SIGTERM).
 * In both cases, we need to be able to clean up the current transaction
 * gracefully, so we can't respond to the interrupt instantaneously ---
 * there's no guarantee that internal data structures would be self-consistent
 * if the code is interrupted at an arbitrary instant.  Instead, the signal
 * handlers set flags that are checked periodically during execution.
 *
 * The CHECK_FOR_INTERRUPTS() macro is called at strategically located spots
 * where it is normally safe to accept a cancel or die interrupt.  In some
 * cases, we invoke CHECK_FOR_INTERRUPTS() inside low-level subroutines that
 * might sometimes be called in contexts that do *not* want to allow a cancel
 * or die interrupt.  The HOLD_INTERRUPTS() and RESUME_INTERRUPTS() macros
 * allow code to ensure that no cancel or die interrupt will be accepted,
 * even if CHECK_FOR_INTERRUPTS() gets called in a subroutine.  The interrupt
 * will be held off until CHECK_FOR_INTERRUPTS() is done outside any
 * HOLD_INTERRUPTS() ... RESUME_INTERRUPTS() section.
 *
 * There is also a mechanism to prevent query cancel interrupts, while still
 * allowing die interrupts: HOLD_CANCEL_INTERRUPTS() and
 * RESUME_CANCEL_INTERRUPTS().
 *
 * Note that ProcessInterrupts() has also acquired a number of tasks that
 * do not necessarily cause a query-cancel-or-die response.  Hence, it's
 * possible that it will just clear InterruptPending and return.
 *
 * INTERRUPTS_PENDING_CONDITION() can be checked to see whether an
 * interrupt needs to be serviced, without trying to do so immediately.
 * Some callers are also interested in INTERRUPTS_CAN_BE_PROCESSED(),
 * which tells whether ProcessInterrupts is sure to clear the interrupt.
 *
 * Special mechanisms are used to let an interrupt be accepted when we are
 * waiting for a lock or when we are waiting for command input (but, of
 * course, only if the interrupt holdoff counter is zero).  See the
 * related code for details.
 *
 * A lost connection is handled similarly, although the loss of connection
 * does not raise a signal, but is detected when we fail to write to the
 * socket. If there was a signal for a broken connection, we could make use of
 * it by setting ClientConnectionLost in the signal handler.
 *
 * A related, but conceptually distinct, mechanism is the "critical section"
 * mechanism.  A critical section not only holds off cancel/die interrupts,
 * but causes any ereport(ERROR) or ereport(FATAL) to become ereport(PANIC)
 * --- that is, a system-wide reset is forced.  Needless to say, only really
 * *critical* code should be marked as a critical section!	Currently, this
 * mechanism is only used for XLOG-related code.
 *
 *****************************************************************************/

/* in globals.c */
/* these are marked volatile because they are set by signal handlers: */
extern PGDLLIMPORT volatile sig_atomic_t InterruptPending;
extern PGDLLIMPORT volatile sig_atomic_t QueryCancelPending;
extern PGDLLIMPORT volatile sig_atomic_t ProcDiePending;
extern PGDLLIMPORT volatile int ProcDieSenderPid;
extern PGDLLIMPORT volatile int ProcDieSenderUid;
extern PGDLLIMPORT volatile sig_atomic_t IdleInTransactionSessionTimeoutPending;
extern PGDLLIMPORT volatile sig_atomic_t TransactionTimeoutPending;
extern PGDLLIMPORT volatile sig_atomic_t IdleSessionTimeoutPending;
extern PGDLLIMPORT volatile sig_atomic_t ProcSignalBarrierPending;
extern PGDLLIMPORT volatile sig_atomic_t LogMemoryContextPending;
extern PGDLLIMPORT volatile sig_atomic_t IdleStatsUpdateTimeoutPending;

extern PGDLLIMPORT volatile sig_atomic_t CheckClientConnectionPending;
extern PGDLLIMPORT volatile sig_atomic_t ClientConnectionLost;

/*****************************************************************************
 *		CHECK_FOR_INTERRUPTS() and friends
 *****************************************************************************/

/*
 * Check whether any enabled interrupt is pending, without trying to service
 * it immediately.  This is can be used in a HOLD_INTERRUPTS() block to check
 * if the HOLD_INTERRUPTS() is delaying the interrupt processing.
 */
#ifndef WIN32
#define INTERRUPTS_PENDING_CONDITION() \
	(unlikely(InterruptPending))
#else
#define INTERRUPTS_PENDING_CONDITION() \
	(unlikely(UNBLOCKED_SIGNAL_QUEUE()) ? \
	 pgwin32_dispatch_queued_signals() : (void) 0, \
	 unlikely(InterruptPending))
#endif

/*
 * Can interrupts be processed in the current state, i.e. are the interrupts
 * not prevented by the HOLD_INTERRUPTS() or a critical section?
 */
#define INTERRUPTS_CAN_BE_PROCESSED() \
	(InterruptHoldoffCount == 0 && CritSectionCount == 0 && \
	 QueryCancelHoldoffCount == 0)

/*
 * Service an interrupt, if one is pending and it's safe to service it now.
 *
 * NB: This is called from all over the codebase, and in fairly tight loops,
 * so this needs to be very short and fast when there is no work to do!
 */
#define CHECK_FOR_INTERRUPTS() \
do { \
	if (INTERRUPTS_PENDING_CONDITION()) \
		ProcessInterrupts(); \
} while(0)

/* in tcop/postgres.c */
extern void ProcessInterrupts(void);

/*****************************************************************************
 *		Critical section and interrupt holdoff mechanism
 *****************************************************************************/

/* these are marked volatile because they are examined by signal handlers: */
extern PGDLLIMPORT volatile uint32 InterruptHoldoffCount;
extern PGDLLIMPORT volatile uint32 QueryCancelHoldoffCount;
extern PGDLLIMPORT volatile uint32 CritSectionCount;

static inline void
HOLD_INTERRUPTS(void)
{
	InterruptHoldoffCount++;
}

static inline void
RESUME_INTERRUPTS(void)
{
	Assert(InterruptHoldoffCount > 0);
	InterruptHoldoffCount--;
}

static inline void
HOLD_CANCEL_INTERRUPTS(void)
{
	QueryCancelHoldoffCount++;
}

static inline void
RESUME_CANCEL_INTERRUPTS(void)
{
	Assert(QueryCancelHoldoffCount > 0);
	QueryCancelHoldoffCount--;
}

static inline void
START_CRIT_SECTION(void)
{
	CritSectionCount++;
}

static inline void
END_CRIT_SECTION(void)
{
	Assert(CritSectionCount > 0);
	CritSectionCount--;
}

#endif							/* IPC_INTERRUPT_H */
