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
 * Note: InterruptMask is defined in waiteventset.h to avoid circular dependency
 */
#include "storage/waiteventset.h"

/*
 * PendingInterrupts is a bitmask representing interrupts that are currently
 * pending for a process, and some flags to help with signaling when the
 * interrupt bits are altered.
 *
 * We support up to 64 different interrupts.  That way, the currently pending
 * interrupts can be conveniently stored as on 64-bit atomic bitmask, on
 * systems with 64-bit atomics.  On other systems, it's split into two 32-bit
 * atomic fields.  That's good enough because we don't rely on atomicity
 * between different interrupt bits.  (Note that the simulated 64-bit atomics
 * are not good enough for this, because the simulation relies on spinlocks,
 * which creates a deadlock risk when used from signal handlers.)
 *
 * Two signaling flags are defined:
 *
 * PI_FLAG_SLEEPING_ON_INTERRUPTS indicates that the backend is currently
 * blocked waiting for an interrupt.  If set, the backend needs to be woken up
 * when a bit in the 'interrupts' mask is set.
 *
 * PI_FLAG_CFI_ATTENTION indicates that some interrupt bits have been changed
 * since the last ProcessInterupts() call.  It's set whenever any 'interrupts'
 * bit is set.  It's checked by CHECK_FOR_INTERRUPTS() as a fastpath check to
 * determine if there can be any interrupts pending that need processing, and
 * cleared in ProcessInterrupts().  It must also be cleared / re-evaluated
 * whenever CheckForInterruptsMask changes (see RECHECK_CFI_ATTENTION()).
 */
typedef struct
{
	pg_atomic_uint32 flags;

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	pg_atomic_uint64 interrupts;
#else
	pg_atomic_uint32 interrupts_lo;
	pg_atomic_uint32 interrupts_hi;
#endif
} PendingInterrupts;

#define PI_FLAG_SLEEPING_ON_INTERRUPTS	0x01
#define PI_FLAG_CFI_ATTENTION			0x02

/*
 * Interrupt vector currently in use for this process.  Most of the time this
 * points to MyProc->pendingInterrupts, but in processes that have no PGPROC
 * entry (yet), it points to a process-private variable, so that interrupts
 * can nevertheless be used from signal handlers in the same process.
 */
extern PGDLLIMPORT PendingInterrupts *MyPendingInterrupts;


/*
 * Interrupt bits that used in PendingIntrrupts->interrupts bitmask.  Each
 * value is a different bit, so that these can be conveniently OR'd together.
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
#define END_ADDIN_INTERRUPTS 64

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
	 * FIXME: that's not true anymore, this is no longer called from
	 * CHECK_FOR_INTERRUPTS(). Still seems correct and a good choice to not
	 * force a barrier here. Just update the comment?
	 *
	 * That means that if the interrupt is concurrently set by another
	 * process, we might miss it. That should be OK, because the next
	 * WaitInterrupt() or equivalent call acts as a synchronization barrier.
	 * We will see the updated value before sleeping.
	 */
	uint64		pending;

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	pending = pg_atomic_read_u64(&MyPendingInterrupts->interrupts);
#else
	pending = (uint64) pg_atomic_read_u32(&MyPendingInterrupts->interrupts_lo);
	pending |= (uint64) pg_atomic_read_u32(&MyPendingInterrupts->interrupts_hi) << 32;
#endif

	return (pending & interruptMask) != 0;
}

/*
 * Clear an interrupt flag (or flags).
 */
static inline void
ClearInterrupt(InterruptMask interruptMask)
{
	uint64		mask = ~interruptMask;

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	(void) pg_atomic_fetch_and_u64(&MyPendingInterrupts->interrupts, mask);
#else
	(void) pg_atomic_fetch_and_u32(&MyPendingInterrupts->interrupts_lo, (uint32) mask);
	(void) pg_atomic_fetch_and_u32(&MyPendingInterrupts->interrupts_hi, (uint32) (mask >> 32));
#endif
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

/*
 * Service an interrupt, if one is pending and it's safe to service it now.
 *
 * NB: This is called from all over the codebase, and in fairly tight loops,
 * so this needs to be very short and fast when there is no work to do!
 */
#define CHECK_FOR_INTERRUPTS()					\
do { \
	if (unlikely(pg_atomic_read_u32(&MyPendingInterrupts->flags) != 0)) \
		ProcessInterrupts();											\
} while(0)

/*
 * Set/clear the PI_FLAG_CFI_ATTENTION according to the currently pending
 * interrupts and CheckForInterruptsMask. This should be called every time
 * after enabling new bits in CheckForInterruptsMask, so that the next
 * CHECK_FOR_INTERRUPTS() will react correctly to the newly enabled
 * interrupts.
 */
static inline void
RECHECK_CFI_ATTENTION(void)
{
	/*
	 * This should not be called while sleeping. No other process sets the
	 * flag, so when we clear/set 'flags' below, we don't need to worry about
	 * overwriting it.
	 */
	Assert((pg_atomic_read_u32(&MyPendingInterrupts->flags) & PI_FLAG_SLEEPING_ON_INTERRUPTS) == 0);

	/* clear the flag first */
	(void) pg_atomic_write_u32(&MyPendingInterrupts->flags, 0);

	/*
	 * Ensure that if a concurrent process sets an interrupt from now on, the
	 * flag will be set again.
	 */
	pg_memory_barrier();

	if (InterruptPending(CheckForInterruptsMask))
		(void) pg_atomic_write_u32(&MyPendingInterrupts->flags, PI_FLAG_CFI_ATTENTION);
}

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
	{
		CheckForInterruptsMask = EnabledInterruptsMask;
		RECHECK_CFI_ATTENTION();
	}
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
	{
		CheckForInterruptsMask = EnabledInterruptsMask;
		RECHECK_CFI_ATTENTION();
	}
	else
		Assert(CheckForInterruptsMask == 0);
}

#endif							/* IPC_INTERRUPT_H */
