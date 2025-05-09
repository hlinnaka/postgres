/*-------------------------------------------------------------------------
 *
 * standard_interrupts.h
 *	  List of built-in interrupt bits
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/ipc/standard_interrupts.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef IPC_STANDARD_INTERRUPTS_H
#define IPC_STANDARD_INTERRUPTS_H

/* defined here to avoid circular dependency to interrupt.h */
typedef uint64 InterruptMask;

/*
 * Interrupt bits that used in PendingIntrrupts->pending_mask bitmask.  Each
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
 * Recovery conflict.  This is sent by the startup process in hot standby mode
 * when a backend holds back the WAL replay for too long.  The reason for the
 * conflict indicated by the PGPROC->pendingRecoveryConflicts bitmask.
 * Conflicts are generally resolved by terminating the current query or
 * session.  The exact reaction depends on the reason and what state the
 * backend is in.
 */
#define INTERRUPT_RECOVERY_CONFLICT 			UINT64_BIT(3)

/*
 * Config file reload is requested.
 *
 * This is normally disabled and therefore not handled at
 * CHECK_FOR_INTERRUPTS().  The "main loop" in each process is expected to
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

/*
 * Indicates whether the startup progress interval mentioned by the user is
 * elapsed or not. TRUE if timeout occurred, FALSE otherwise.
 */
#define INTERRUPT_STARTUP_PROGRESS_TIMER_EXPIRED	UINT64_BIT(11)

/* Raised by timer while idle, to send a stats update */
#define INTERRUPT_IDLE_STATS_TIMEOUT			UINT64_BIT(12)

/* Raised synchronously when the client connection is lost */
#define INTERRUPT_CLIENT_CONNECTION_LOST		 UINT64_BIT(13)

/*
 * INTERRUPT_ASYNC_NOTIFY is sent to notify backends that have registered to
 * LISTEN on any channels that they might have messages they need to deliver
 * to the frontend.  It is also processed whenever starting to read from the
 * client or while doing so, but only when there is no transaction in
 * progress.
 */
#define INTERRUPT_ASYNC_NOTIFY					UINT64_BIT(14)

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
#define INTERRUPT_SINVAL_CATCHUP				UINT64_BIT(15)

/* Message from a cooperating parallel backend or apply worker */
#define INTERRUPT_PARALLEL_MESSAGE				UINT64_BIT(16)


/***********************************************************************
 * Process-specific interrupts
 *
 * Some processes need dedicated interrupts for various purposes.  Ignored
 * by other processes.
 ***********************************************************************/

/* ask walsenders to prepare for shutdown  */
#define INTERRUPT_WALSND_INIT_STOPPING			UINT64_BIT(17)

/* TODO: document the difference with INTERRUPT_WALSND_INIT_STOPPING */
#define INTERRUPT_WALSND_STOP					UINT64_BIT(18)

/*
 * INTERRUPT_WAL_ARRIVED is used to wake up the startup process, to tell it
 * that it should continue WAL replay.  It's sent by WAL receiver when more
 * WAL arrives, or when promotion is requested.
 */
#define INTERRUPT_WAL_ARRIVED					UINT64_BIT(19)

/*
 * Wake up startup process to check for the promotion signal file
 *
 * Also used to request a slotsync worker or some other backend syncing
 * replication slots to stop syncing.
 */
#define INTERRUPT_CHECK_PROMOTE					UINT64_BIT(20)

/* sent to logical replication launcher, when a subscription changes */
#define INTERRUPT_SUBSCRIPTION_CHANGE			UINT64_BIT(21)

/* Graceful shutdown request for a parallel apply worker */
#define INTERRUPT_SHUTDOWN_PARALLEL_APPLY_WORKER UINT64_BIT(22)

/* Request checkpointer to perform one last checkpoint, then shut down */
#define INTERRUPT_SHUTDOWN_XLOG					UINT64_BIT(23)

#define INTERRUPT_SHUTDOWN_PGARCH				UINT64_BIT(24)

/*
 * This is sent to the autovacuum launcher when an autovacuum worker exits
 */
#define INTERRUPT_AUTOVACUUM_WORKER_FINISHED 	UINT64_BIT(25)


/***********************************************************************
 * End of built-in interrupt bits
 *
 * The remaining bits are handed out by RequestAddinInterrupt, for
 * extensions
 ***********************************************************************/
#define BEGIN_ADDIN_INTERRUPTS 26
#define END_ADDIN_INTERRUPTS 64


/* for extensions */
extern InterruptMask RequestAddinInterrupt(void);

/* Standard interrupt handling functions.  Defined in tcop/postgres.c */
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

#endif							/* IPC_STANDARD_INTERRUPTS_H */
