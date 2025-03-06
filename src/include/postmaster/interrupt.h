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
 * handlers, or "sent" by other backends setting them directly.
 *
 * Standard interrupts
 * -------------------
 *
 * Some interrupts need to be processed fairly quickly even when the backend
 * is busy, like QueryCancel (SIGINT) and ProcDie (SIGTERM), but that requires
 * cleaning up the current transaction gracefully, and there's no guarantee
 * that internal data structures would be self-consistent if the code is
 * interrupted at an arbitrary instant.
 *
 * The CHECK_FOR_INTERRUPTS() macro is called at strategically located spots
 * where it is normally safe to accept a cancel or die interrupt.
 * Well-behaved backend code performs CHECK_FOR_INTERRUPTS() periodically in
 * long computations, and should never sleep using mechanisms other than the
 * WaitEventSet mechanism or the more convenient WaitInterrupt /
 * WaitSockerOrInterrupt functions (except for bounded short periods, eg
 * LWLock waits), so they should react in good time.
 *
 * When you need to wait, pass INTERRUPT_CFI_MASK() to WaitInterrupt() and
 * call CHECK_FOR_INTERRUPTS() every time WaitInterrupt() returns.
 *
 * In some cases, we invoke CHECK_FOR_INTERRUPTS() inside low-level
 * subroutines that might sometimes be called in contexts that do *not* want
 * to allow a cancel or die interrupt.  The HOLD_INTERRUPTS() and
 * RESUME_INTERRUPTS() macros allow code to ensure that no cancel or die
 * interrupt will be accepted, even if CHECK_FOR_INTERRUPTS() gets called in a
 * subroutine.  The interrupt will be held off until CHECK_FOR_INTERRUPTS() is
 * done outside any HOLD_INTERRUPTS() ... RESUME_INTERRUPTS() section.  There
 * is also a mechanism to prevent query cancel interrupts, while still
 * allowing die interrupts: HOLD_CANCEL_INTERRUPTS() and
 * RESUME_CANCEL_INTERRUPTS().
 *
 * Note that ProcessInterrupts() has also acquired a number of tasks that do
 * not necessarily cause a query-cancel-or-die response.  Hence, it's possible
 * that it will just clear some interrupt bits and return.
 *
 * INTERRUPTS_PENDING_CONDITION() can be checked to see whether an
 * interrupt needs to be serviced, without trying to do so immediately.
 * Some callers are also interested in INTERRUPTS_CAN_BE_PROCESSED(),
 * which tells whether ProcessInterrupts is sure to clear the interrupt.
 *
 * Special mechanisms are used to let an interrupt be accepted when we are
 * waiting for a lock or when we are waiting for command input (but, of
 * course, only if the interrupt holdoff counter is zero).  See the related
 * code for details.
 *
 * A lost connection is handled similarly to a ProcDie request, although the
 * loss of connection is detected and the interrupt is raied when we fail to
 * write to the socket. If there was a signal for a broken connection, we
 * could make use of it by setting ClientConnectionLost in the signal handler.
 *
 * Standard interrupts in AUX processes
 * ------------------------------------
 *
 * In background processes that are not regular backends, responses to signals
 * that are translated to interrupts are fairly varied and many types of
 * backends have their own implementations. Some use CHECK_FOR_INTERRUPTS()
 * but have additional interrupts that are also processed in the main
 * loop. Others don't use CHECK_FOR_INTERRUPTS() at all, but have their own
 * Process*Interrupts() function that is called at strategic spots.
 *
 * We nevertheless provide a few generic signal handlers and interrupt checks
 * to facilitate code reuse, see ProcessMainLoopInterrupts() and the standard
 * signal handlers SignalHandlerForConfigReload(), SignalHandlerForCrashExit(),
 * and SignalHandlerForShutdownRequest().
 *
 * INTERRUPT_GENERAL: The multiplexed general-purpose wakeup
 * ---------------------------------------------------------
 *
 * The INTERRUPT_GENERAL interrupt is multiplexed for many different purposes
 * that don't warrant a dedicated interrupt bit.  Because it's reused for
 * different purposes, waiters must tolerate receiving spurious interrupt
 * wakeups.
 *
 * Waiting on an interrupt
 * -----------------------
 *
 * The correct pattern to wait for event(s) using INTERRUPT_GENERAL (or any
 * bespoken interrupt flag) is:
 *
 * for (;;)
 * {
 *	   CHECK_FOR_INTERRUPTS();
 *
 *	   ClearInterrupt(INTERRUPT_GENERAL);
 *	   if (work to do)
 *		   Do Stuff();
 *	   WaitInterrupt(INTERRUPT_CFI_MASAK() | INTERRUPT_GENERAL, ...);
 * }
 *
 * It's important to clear the interrupt *before* checking if there's work to
 * do.  Otherwise, if someone sets the interrupt between the check and the
 * ClearInterrupt() call, you will miss it and Wait will incorrectly block.
 *
 * Another valid coding pattern looks like:
 *
 * for (;;)
 * {
 *	   CHECK_FOR_INTERRUPTS();
 *
 *	   if (work to do)
 *		   Do Stuff(); // in particular, exit loop if some condition satisfied
 *	   WaitInterrupt(INTERRUPT_CFI_MASK() | INTERRUPT_GENERAL, ...);
 *	   ClearInterrupt(INTERRUPT_GENERAL);
 * }
 *
 * This is useful to reduce interrupt traffic if it's expected that the loop's
 * termination condition will often be satisfied in the first iteration; the
 * cost is an extra loop iteration before blocking when it is not.  What must
 * be avoided is placing any checks for asynchronous events after
 * WaitInterrupt and before ClearInterrupt, as that creates a race condition.
 *
 * To wake up a process waiting on INTERRUPT_GENERAL, you must first set a
 * global flag or something else that the wait loop tests in the "if (work to
 * do)" part, and call SendInterrupt(INTERRUPT_GENERAL) *after*
 * that. SendInterrupt is designed to return quickly if the interrupt is
 * already set.
 *
 * In more complex scenarios with nested loops that can consume different
 * events, you can define your own INTERRUPT_* flag instead of relying on
 * INTERRUPT_GENERAL.
 *
 * Standard Signal handlers
 * ------------------------
 *
 * Responses to signals that are translated to interrupts are fairly varied
 * and many types of backends have their own implementations, but we provide a
 * few generic signal handlers and interrupt checks to facilitate code reuse.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/postmaster/interrupt.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef INTERRUPT_H
#define INTERRUPT_H

#include "port/atomics.h"
#include "storage/procnumber.h"
#include "storage/waiteventset.h"		/* WL_* are defined in waiteventset.h */

extern PGDLLIMPORT pg_atomic_uint32 *MyPendingInterrupts;

/* these are marked volatile because they are examined by signal handlers: */
/* FIXME: is that still true, do these still need to be volatile? */
extern PGDLLIMPORT volatile uint32 InterruptHoldoffCount;
extern PGDLLIMPORT volatile uint32 QueryCancelHoldoffCount;
extern PGDLLIMPORT volatile uint32 CritSectionCount;

/*
 * If you called ProcessInterrupts() now, it would process and clear these
 * interrupts.
 */
#define INTERRUPT_CFI_MASK() \
	( \
		(InterruptHoldoffCount > 0 || CritSectionCount > 0) ? 0 :		\
		((QueryCancelHoldoffCount > 0) ? (INTERRUPT_CFI_ALL_MASK & (~INTERRUPT_QUERY_CANCEL)) : \
		 INTERRUPT_CFI_ALL_MASK)											\
		)

/*
 * Service standard interrupts, if one is pending and it's safe to service it now.
 *
 * This could check INTERRUPT_CFI_MASK(), but we prefer to keep his short and
 * fast in the common case that no interrupts are pending. That's why this
 * checks the constant INTERRUPT_CFI_ALL_MASK instead and check the holdoffs
 * are out-of-line in ProcessInterrupts().
 */
#define CHECK_FOR_INTERRUPTS() \
do { \
	if (IsInterruptPending(INTERRUPT_CFI_ALL_MASK)) \
		ProcessInterrupts(); \
} while(0)

/* Would ProcessInterrupts() clear all the bits in 'mask'? */
#define INTERRUPTS_CAN_BE_PROCESSED(mask) \
	(((mask) & ~INTERRUPT_CFI_MASK()) == 0)

#define HOLD_INTERRUPTS() \
do { \
	InterruptHoldoffCount++; \
} while(0)

#define RESUME_INTERRUPTS() \
do { \
	Assert(InterruptHoldoffCount > 0); \
	InterruptHoldoffCount--; \
} while(0)

#define HOLD_CANCEL_INTERRUPTS()  (QueryCancelHoldoffCount++)

#define RESUME_CANCEL_INTERRUPTS() \
do { \
	Assert(QueryCancelHoldoffCount > 0); \
	QueryCancelHoldoffCount--; \
} while(0)


/*
 * Flags in the pending interrupts bitmask. Each value represents one bit in
 * the bitmask.
 */
typedef enum InterruptType
{
	/*
	 * INTERRUPT_GENERAL is used as a general-purpose wakeup, multiplexed for
	 * many reasons.
	 */
	INTERRUPT_GENERAL = 1 << 0,

	/*
	 * Because backends sitting idle will not be reading sinval events, we
	 * need a way to give an idle backend a swift kick in the rear and make it
	 * catch up before the sinval queue overflows and forces it to go through
	 * a cache reset exercise.  This is done by sending
	 * INTERRUPT_SINVAL_CATCHUP to any backend that gets too far behind.
	 *
	 * The interrupt is processed whenever starting to read from the client,
	 * or when interrupted while doing so, ProcessClientReadInterrupt() will
	 * call ProcessCatchupInterrupt().
	 */
	INTERRUPT_SINVAL_CATCHUP = 1 << 1,

	/*
	 * INTERRUPT_ASYNC_NOTIFY is sent to notify backends that have registered
	 * to LISTEN on any channels that they might have messages they need to
	 * deliver to the frontent. It is also processed whenever starting to read
	 * from the client or while doing so, but only when there is no
	 * transaction in progress.
	 */
	INTERRUPT_ASYNC_NOTIFY = 1 << 2,

	/* Raised by timer while idle, to send a stats update */
	INTERRUPT_IDLE_STATS_TIMEOUT = 1 << 3,

	/* Config file reload is requested */
	INTERRUPT_CONFIG_RELOAD = 1 << 4,

	/*
	 * INTERRUPT_RECOVERY_CONTINUE is used to wake up the startup process, to
	 * tell it that it should continue WAL replay.  It's sent by WAL receiver
	 * when more WAL arrives, or when promotion is requested.  We don't reuse
	 * INTERRUPT_GENERAL for this, so that more WAL arriving doesn't wake up
	 * the startup process excessively when we're waiting in other places,
	 * like for recovery conflicts.
	 */
	INTERRUPT_RECOVERY_CONTINUE = 1 << 5,

	/* sent to logical replication launcher, when a subscription changes */
	INTERRUPT_SUBSCRIPTION_CHANGE = 1 << 6,

	/*
	 * Many aux processes don't want to react to INTERRUPT_DIE in
	 * CHECK_FOR_INTERRUPTS(), so they use a separate flag when shutdown is
	 * requested.
	 *
	 * TODO: perhaps use INTERRUPT_DIE, but teach CHECK_FOR_INTERRUPTS() to
	 * ignore it in aux processes, and remove it from CheckForInterruptsMask.
	 * That would save one interrupt bit, and would make things more
	 * consistent.
	 */
	INTERRUPT_SHUTDOWN_AUX = 1 << 7,

	/*
	 * Perform one last checkpoint, then shut down. Only used in the checkpointer
	 * process.
	 */
	INTERRUPT_SHUTDOWN_XLOG = 1 << 8,

	/*---- Interrupts handled by CHECK_FOR_INTERRUPTS() ----*/

	/*
	 * Backend has been requested to terminate
	 *
	 * This is raised by the SIGTERM signal handler, or can be sent directly
	 * by another backend e.g. with pg_terminate_backend().
	 */
	INTERRUPT_DIE = 1 << 9,

	/*
	 * Cancel current query, if any.
	 *
	 * This is raised by the SIGTERM signal handler, or can be sent directly
	 * by another backend e.g. with pg_cancel_backend(), or in response to a
	 * query cancellation packet.
	 */
	INTERRUPT_QUERY_CANCEL = 1 << 10,

	/* ask walsenders to prepare for shutdown  */
	INTERRUPT_WALSND_INIT_STOPPING = 1 << 11,

	/*
	 * Recovery conflict reasons. These are sent by the startup process in hot
	 * standby mode when a backend holds back the WAL replay for too long.
	 */
	INTERRUPT_RECOVERY_CONFLICT_DATABASE = 1 << 12,
	INTERRUPT_RECOVERY_CONFLICT_TABLESPACE = 1 << 13,
	INTERRUPT_RECOVERY_CONFLICT_LOCK = 1 << 14,
	INTERRUPT_RECOVERY_CONFLICT_SNAPSHOT = 1 << 15,
	INTERRUPT_RECOVERY_CONFLICT_BUFFERPIN = 1 << 16,
	INTERRUPT_RECOVERY_CONFLICT_STARTUP_DEADLOCK = 1 << 17,
	INTERRUPT_RECOVERY_CONFLICT_LOGICALSLOT = 1 << 18,

	/* Raised by timers */
	INTERRUPT_TRANSACTION_TIMEOUT = 1 << 19,
	INTERRUPT_IDLE_SESSION_TIMEOUT = 1 << 20,
	INTERRUPT_IDLE_IN_TRANSACTION_SESSION_TIMEOUT = 1 << 21,
	INTERRUPT_CLIENT_CHECK_TIMEOUT = 1 << 22,

	/* Raised synchronously when the client connection is lost */
	INTERRUPT_CLIENT_CONNECTION_LOST = 1 << 23,

	/* Ask backend to log the memory contexts */
	INTERRUPT_LOG_MEMORY_CONTEXT = 1 << 24,

	/* Message from a cooperating parallel backend */
	INTERRUPT_PARALLEL_MESSAGE = 1 << 25,

	/* Message from a parallel apply worker */
	INTERRUPT_PARALLEL_APPLY_MESSAGE = 1 << 26,

	/* procsignal global barrier interrupt  */
	INTERRUPT_BARRIER = 1 << 27,

	/*---- end of interrupts handled by CHECK_FOR_INTERRUPTS() ----*/

	/*
	 * NOTE: InterruptTypes must fit in a 32-bit bitmask. (If we had efficient
	 * 64-bit atomics on all platforms, we could easily go up to 64 bits)
	 */

	/*
	 * SLEEPING_ON_INTERRUPTS indicates that the backend is currently blocked
	 * waiting for an interrupt. If it's set, the backend needs to be woken up
	 * when a bit in the pending interrupts mask is set. It's used internally
	 * by the interrupt machinery, and cannot be used directly in the public
	 * functions. It's named differently to distinguish it from the actual
	 * interrupt flags.
	 */
	SLEEPING_ON_INTERRUPTS = 1 << 31,

} InterruptType;

/* The set of interrupts that are (ever) processed by CHECK_FOR_INTERRUPTS */
#define INTERRUPT_CFI_ALL_MASK	(						\
		INTERRUPT_DIE |									\
		INTERRUPT_QUERY_CANCEL |						\
		INTERRUPT_WALSND_INIT_STOPPING |				\
		INTERRUPT_RECOVERY_CONFLICT_DATABASE |			\
		INTERRUPT_RECOVERY_CONFLICT_TABLESPACE |		\
		INTERRUPT_RECOVERY_CONFLICT_LOCK |				\
		INTERRUPT_RECOVERY_CONFLICT_SNAPSHOT |			\
		INTERRUPT_RECOVERY_CONFLICT_BUFFERPIN |			\
		INTERRUPT_RECOVERY_CONFLICT_STARTUP_DEADLOCK |	\
		INTERRUPT_RECOVERY_CONFLICT_LOGICALSLOT |		\
		INTERRUPT_TRANSACTION_TIMEOUT |					\
		INTERRUPT_IDLE_SESSION_TIMEOUT |				\
		INTERRUPT_IDLE_IN_TRANSACTION_SESSION_TIMEOUT |	\
		INTERRUPT_CLIENT_CHECK_TIMEOUT |				\
		INTERRUPT_CLIENT_CONNECTION_LOST |				\
		INTERRUPT_LOG_MEMORY_CONTEXT |					\
		INTERRUPT_PARALLEL_MESSAGE |					\
		INTERRUPT_PARALLEL_APPLY_MESSAGE |				\
		INTERRUPT_BARRIER								\
		)

/* The set of interrupts that are processed by ProcessMainLoopInterrupts */
#define INTERRUPT_MAIN_LOOP_MASK	(			\
		INTERRUPT_BARRIER |						\
		INTERRUPT_SHUTDOWN_AUX |				\
		INTERRUPT_LOG_MEMORY_CONTEXT |			\
		INTERRUPT_CONFIG_RELOAD					\
		)

/*
 * Test an interrupt flag (or flags).
 */
static inline bool
IsInterruptPending(uint32 interruptMask)
{
	pg_read_barrier();

#ifdef WIN32
	if (unlikely(UNBLOCKED_SIGNAL_QUEUE()))
		pgwin32_dispatch_queued_signals();
#endif

	if (unlikely((pg_atomic_read_u32(MyPendingInterrupts) & interruptMask) != 0))
		return true;
	else
		return false;
}

/*
 * Clear an interrupt flag (or flags).
 */
static inline void
ClearInterrupt(uint32 interruptMask)
{
	pg_atomic_fetch_and_u32(MyPendingInterrupts, ~interruptMask);
	pg_write_barrier();
}

/*
 * Test and clear an interrupt flag (or flags).
 */
static inline bool
ConsumeInterrupt(uint32 interruptMask)
{
	if (likely(!IsInterruptPending(interruptMask)))
		return false;

	ClearInterrupt(interruptMask);

	return true;
}

extern void RaiseInterrupt(uint32 interruptMask);
extern void SendInterrupt(uint32 interruptMask, ProcNumber pgprocno);
extern int	WaitInterrupt(uint32 interruptMask, int wakeEvents, long timeout,
						  uint32 wait_event_info);
extern int	WaitInterruptOrSocket(uint32 interruptMask, int wakeEvents, pgsocket sock,
								  long timeout, uint32 wait_event_info);
extern void SwitchToLocalInterrupts(void);
extern void SwitchToSharedInterrupts(void);
extern void InitializeInterruptWaitSet(void);

extern void ProcessMainLoopInterrupts(void);
extern void SignalHandlerForConfigReload(SIGNAL_ARGS);
extern void SignalHandlerForCrashExit(SIGNAL_ARGS);
extern void SignalHandlerForShutdownRequest(SIGNAL_ARGS);

/* in tcop/postgres.c */
extern void ProcessInterrupts(void);

#endif
