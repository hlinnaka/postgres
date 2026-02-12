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

#include "port/atomics.h"
#include "storage/procnumber.h"

/*
 * Include waiteventset.h for the WL_* flags. They're not needed her, but are
 * needed which are needed by all callers of WaitInterrupt, so include it
 * here.
 *
 * Note: InterruptMask is defind in waiteventset.h to avoid circular dependency
 */
#include "storage/waiteventset.h"


/*
 * Flags in the pending interrupts bitmask. Each value is a different bit, so that
 * these can be conveniently OR'd together.
 */
#define UINT64_BIT(shift) (UINT64_C(1) << (shift))

/***********************************************************************
 * Begin definitions of built-in interrupt bits
 ***********************************************************************/

/*
 * INTERRUPT_WAIT_WAKEUP is shared by many use cases that need to wake up
 * a process, which don't need a dedicated interrupt bit.
 */
#define INTERRUPT_WAIT_WAKEUP					UINT64_BIT(0)


/***********************************************************************
 * Standard interrupts handled the same by most processes
 *
 * Most of these are normally processed by CHECK_FOR_INTERRUPTS() once
 * process startup has reached SetStandardInterrupts().
 ***********************************************************************/

/*
 * Backend has been requested to terminate gracefully.
 *
 * This is raised by the SIGTERM signal handler, or can be sent directly by
 * another backend e.g. with pg_terminate_backend().
 */
#define INTERRUPT_TERMINATE						UINT64_BIT(1)

/*
 * Cancel current query, if any.
 *
 * Sent to regular backends by pg_cancel_backend(), SIGINT, or in response to
 * a query cancellation packet.  Some other processes like autovacuum workers
 * and logical decoding processes also react to this.
 */
#define INTERRUPT_QUERY_CANCEL					UINT64_BIT(2)

/*
 * Recovery conflict. This is sent by the startup process in hot standby mode
 * when a backend holds back the WAL replay for too long. The reason for the
 * conflict indicated by the PGPROC->pendingRecoveryConflicts
 * bitmask. Conflicts are generally resolved by terminating the current query
 * or session. The exact reaction depends on the reason and what state the
 * backend is in.
 */
#define INTERRUPT_RECOVERY_CONFLICT 			UINT64_BIT(3)

/*
 * Config file reload is requested.
 *
 * This is normally disabled and therefore not handled at
 * CHECK_FOR_INTERRUPTS(). The "main loop" in each process is expected to
 * check for it explicitly.
 */
#define INTERRUPT_CONFIG_RELOAD					UINT64_BIT(4)

/*
 * Log current memory contexts, sent by pg_log_backend_memory_contexts()
 */
#define INTERRUPT_LOG_MEMORY_CONTEXT 			UINT64_BIT(5)

/*
 * procsignal global barrier interrupt
 */
#define INTERRUPT_BARRIER						UINT64_BIT(6)


/***********************************************************************
 * Interrupts used by client backends and most other processes that
 * connect to a particular database.
 *
 * Most of these are also processed by CHECK_FOR_INTERRUPTS() once process
 * startup has reached SetStandardInterrupts().
 ***********************************************************************/

/* Raised by timers */
#define INTERRUPT_TRANSACTION_TIMEOUT			UINT64_BIT(7)
#define INTERRUPT_IDLE_SESSION_TIMEOUT			UINT64_BIT(8)
#define INTERRUPT_IDLE_IN_TRANSACTION_SESSION_TIMEOUT UINT64_BIT(9)
#define INTERRUPT_CLIENT_CHECK_TIMEOUT			UINT64_BIT(10)

/* Raised by timer while idle, to send a stats update */
#define INTERRUPT_IDLE_STATS_TIMEOUT			UINT64_BIT(11)

/* Raised synchronously when the client connection is lost */
#define INTERRUPT_CLIENT_CONNECTION_LOST		 UINT64_BIT(12)

/*
 * INTERRUPT_ASYNC_NOTIFY is sent to notify backends that have registered to
 * LISTEN on any channels that they might have messages they need to deliver
 * to the frontend. It is also processed whenever starting to read from the
 * client or while doing so, but only when there is no transaction in
 * progress.
 */
#define INTERRUPT_ASYNC_NOTIFY					UINT64_BIT(13)

/*
 * Because backends sitting idle will not be reading sinval events, we need a
 * way to give an idle backend a swift kick in the rear and make it catch up
 * before the sinval queue overflows and forces it to go through a cache reset
 * exercise.  This is done by sending INTERRUPT_SINVAL_CATCHUP to any backend
 * that gets too far behind.
 *
 * The interrupt is processed whenever starting to read from the client, or
 * when interrupted while doing so.
 */
#define INTERRUPT_SINVAL_CATCHUP				UINT64_BIT(14)

/* Message from a cooperating parallel backend or apply worker */
#define INTERRUPT_PARALLEL_MESSAGE				UINT64_BIT(15)


/***********************************************************************
 * Process-specific interrupts
 *
 * Some processes need dedicated interrupts for various purposes.  Ignored
 * by other processes.
 ***********************************************************************/

/* ask walsenders to prepare for shutdown  */
#define INTERRUPT_WALSND_INIT_STOPPING			UINT64_BIT(16)

/* TODO: document the difference with INTERRUPT_WALSND_INIT_STOPPING */
#define INTERRUPT_WALSND_STOP					UINT64_BIT(17)

/*
 * INTERRUPT_WAL_ARRIVED is used to wake up the startup process, to tell
 * it that it should continue WAL replay. It's sent by WAL receiver when
 * more WAL arrives, or when promotion is requested.
 */
#define INTERRUPT_WAL_ARRIVED					UINT64_BIT(18)

/* Wake up startup process to check for the promotion signal file */
#define INTERRUPT_CHECK_PROMOTE					UINT64_BIT(19)

/* sent to logical replication launcher, when a subscription changes */
#define INTERRUPT_SUBSCRIPTION_CHANGE			UINT64_BIT(20)

/* Graceful shutdown request for a parallel apply worker */
#define INTERRUPT_SHUTDOWN_PARALLEL_APPLY_WORKER UINT64_BIT(21)

/* Request checkpointer to perform one last checkpoint, then shut down. */
#define INTERRUPT_SHUTDOWN_XLOG					UINT64_BIT(22)

#define INTERRUPT_SHUTDOWN_PGARCH				UINT64_BIT(23)

/*
 * This is sent to the autovacuum launcher when an autovacuum worker exits
 */
#define INTERRUPT_AUTOVACUUM_WORKER_FINISHED 	UINT64_BIT(24)


/***********************************************************************
 * End of built-in interrupt bits
 *
 * The remaining bits are handed out by RequestAddinInterrupt, for
 * extensions
 ***********************************************************************/
#define BEGIN_ADDIN_INTERRUPTS 25
#define END_ADDIN_INTERRUPTS 63

/*
 * SLEEPING_ON_INTERRUPTS indicates that the backend is currently blocked
 * waiting for an interrupt.  If set, the backend needs to be woken up when a
 * bit in the pending interrupts mask is set.  It's used internally by the
 * interrupt machinery, and cannot be used directly in the public functions.
 * It's named differently to distinguish it from the actual interrupt flags.
 */
#define SLEEPING_ON_INTERRUPTS UINT64_BIT(63)

extern PGDLLIMPORT pg_atomic_uint64 *MyPendingInterrupts;

/*
 * Test an interrupt flag (or flags).
 */
static inline bool
InterruptPending(InterruptMask interruptMask)
{
	/*
	 * Note that there is no memory barrier here. This is used in
	 * CHECK_FOR_INTERRUPTS(), so we want this to be as cheap as possible.
	 *
	 * That means that if the interrupt is concurrently set by another
	 * process, we might miss it. That should be OK, because the next
	 * WaitInterrupt() or equivalent call acts as a synchronization barrier.
	 * We will see the updated value before sleeping.
	 */
	return (pg_atomic_read_u64(MyPendingInterrupts) & interruptMask) != 0;
}

/*
 * Clear an interrupt flag (or flags).
 */
static inline void
ClearInterrupt(InterruptMask interruptMask)
{
	pg_atomic_fetch_and_u64(MyPendingInterrupts, ~interruptMask);
}

/*
 * Test and clear an interrupt flag (or flags).
 */
static inline bool
ConsumeInterrupt(InterruptMask interruptMask)
{
	if (likely(!InterruptPending(interruptMask)))
		return false;

	ClearInterrupt(interruptMask);
	return true;
}

extern void RaiseInterrupt(InterruptMask interruptMask);
extern void SendInterrupt(InterruptMask interruptMask, ProcNumber pgprocno);
extern int	WaitInterrupt(InterruptMask interruptMask, int wakeEvents, long timeout,
						  uint32 wait_event_info);
extern int	WaitInterruptOrSocket(InterruptMask interruptMask, int wakeEvents, pgsocket sock,
								  long timeout, uint32 wait_event_info);
extern void SwitchToLocalInterrupts(void);
extern void SwitchToSharedInterrupts(void);
extern void InitializeInterruptWaitSet(void);

typedef void (*pg_interrupt_handler_t) (void);
extern void SetInterruptHandler(InterruptMask interruptMask, pg_interrupt_handler_t handler);

extern void EnableInterrupt(InterruptMask interruptMask);
extern void DisableInterrupt(InterruptMask interruptMask);

/* for extensions */
extern InterruptMask RequestAddinInterrupt(void);

/* Standard interrupt handlers. Defined in tcop/postgres.c */
extern void SetStandardInterruptHandlers(void);

extern void ProcessQueryCancelInterrupt(void);
extern void ProcessTerminateInterrupt(void);
extern void ProcessConfigReloadInterrupt(void);
extern void ProcessAsyncNotifyInterrupt(void);
extern void ProcessIdleStatsTimeoutInterrupt(void);
extern void ProcessRecoveryConflictInterrupts(void);
extern void ProcessTransactionTimeoutInterrupt(void);
extern void ProcessIdleSessionTimeoutInterrupt(void);
extern void ProcessIdleInTransactionSessionTimeoutInterrupt(void);
extern void ProcessClientCheckTimeoutInterrupt(void);
extern void ProcessClientConnectionLost(void);

extern void ProcessAuxProcessShutdownInterrupt(void);


/*****************************************************************************
 *	  CHECK_FOR_INTERRUPTS() and friends
 *****************************************************************************/


extern PGDLLIMPORT volatile InterruptMask EnabledInterruptsMask;
extern PGDLLIMPORT volatile InterruptMask CheckForInterruptsMask;

extern void ProcessInterrupts(void);

/* Test whether an interrupt is pending */
#ifndef WIN32
#define INTERRUPTS_PENDING_CONDITION(mask) \
	(unlikely(InterruptPending(mask)))
#else
#define INTERRUPTS_PENDING_CONDITION(mask) \
	(unlikely(UNBLOCKED_SIGNAL_QUEUE()) ? \
	 pgwin32_dispatch_queued_signals() : (void) 0,	\
	 unlikely(InterruptPending(mask)))
#endif

/*
 * Is ProcessInterrupts() guaranteed to clear all the bits in 'mask'?
 *
 * (The interrupt handler may re-raise the interrupt, though)
 */
#define INTERRUPTS_CAN_BE_PROCESSED(mask) \
	(((mask) & CheckForInterruptsMask) == (mask))

/* Service interrupt, if one is pending and it's safe to service it now */
#define CHECK_FOR_INTERRUPTS()					\
do { \
	if (INTERRUPTS_PENDING_CONDITION(CheckForInterruptsMask)) \
		ProcessInterrupts();											\
} while(0)


/*****************************************************************************
 *	  Critical section and interrupt holdoff mechanism
 *****************************************************************************/

/* these are marked volatile because they are examined by signal handlers: */
/*
 * XXX: is that still true? Should we use local vars to avoid repeated access
 * e.g. inside RESUME_INTERRUPTS() ?
 */
extern PGDLLIMPORT volatile uint32 InterruptHoldoffCount;
extern PGDLLIMPORT volatile uint32 CritSectionCount;

static inline void
HOLD_INTERRUPTS(void)
{
	InterruptHoldoffCount++;
	CheckForInterruptsMask = (InterruptMask) 0;
}

static inline void
RESUME_INTERRUPTS(void)
{
	Assert(CheckForInterruptsMask == 0);
	Assert(InterruptHoldoffCount > 0);
	InterruptHoldoffCount--;
	if (InterruptHoldoffCount == 0 && CritSectionCount == 0)
		CheckForInterruptsMask = EnabledInterruptsMask;
	else
		Assert(CheckForInterruptsMask == 0);
}

static inline void
START_CRIT_SECTION(void)
{
	CritSectionCount++;
	CheckForInterruptsMask = 0;
}

static inline void
END_CRIT_SECTION(void)
{
	Assert(CritSectionCount > 0);
	CritSectionCount--;
	if (InterruptHoldoffCount == 0 && CritSectionCount == 0)
		CheckForInterruptsMask = EnabledInterruptsMask;
	else
		Assert(CheckForInterruptsMask == 0);
}

#endif							/* IPC_INTERRUPT_H */
