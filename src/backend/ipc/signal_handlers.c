/*-------------------------------------------------------------------------
 *
 * signal_handlers.c
 *	  Standard signal handlers.
 *
 * These just raise the corresponding INTERRUPT_* flags:
 *
 * SIGHUP ->  request config reload
 * SIGINT == query cancel
 * SIGTERM == graceful terminate of the process
 * SIGQUIT == exit immediately (causes crash restart)
 *
 * XXX: We should not send signals directly between processes anymore. Use
 * SendInterrupt instead.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/ipc/signal_handlers.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "ipc/interrupt.h"
#include "ipc/signal_handlers.h"
#include "libpq/pqsignal.h"

/*
 * Set the standard signal handlers suitable for most postmaster child
 * processes.
 *
 * Note: this doesn't unblock the signals yet. You can make additional
 * pqsignal() calls to modify the default behavior before unblocking.
 */
void
SetPostmasterChildSignalHandlers(void)
{
	/*----------
	 * These raise the corresponding interrupts in the process:
	 *
	 * SIGHUP -> INTERRUPT_CONFIG_RELOAD
	 * SIGINT -> INTERRUPT_QUERY_CANCEL
	 * SIGTERM -> INTERRUPT_TERMINATE
	 *
	 * The process may ignore the interrupts that these raise, e.g if query
	 * cancellation is not applicable.  But there's no harm in having the
	 * signal handlers in place anyway.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SignalHandlerForQueryCancel);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, SIG_IGN);

	/*
	 * SIGUSR2 is sent by postmaster to some aux processes, for different
	 * purposes.  Such processes override this before unblocking signals, but
	 * ignore it by default.
	 */
	pqsignal(SIGUSR2, SIG_IGN);

	/* FIXME: should we do this in all processes? */
	/* pqsignal(SIGFPE, FloatExceptionHandler); */

	/*
	 * SIGALRM is used for timeouts, but the handler is established later in
	 * InitializeTimeouts()
	 */
	pqsignal(SIGALRM, SIG_IGN);

	/* SIGURG is handled in waiteventset.c */

	/*
	 * Reset signals that are used by postmaster but not by child processes.
	 *
	 * Currently just SIGCHLD.  The handlers for other signals are overridden
	 * later, depending on the child process type.
	 */
	pqsignal(SIGCHLD, SIG_DFL); /* system() requires this to be SIG_DFL rather
								 * than SIG_IGN on some platforms */

	/*
	 * Every postmaster child process is expected to respond promptly to
	 * SIGQUIT at all times.  Therefore we centrally remove SIGQUIT from
	 * BlockSig and install a suitable signal handler.  (Client-facing
	 * processes may choose to replace this default choice of handler with
	 * quickdie().)  All other blockable signals remain blocked for now.
	 */
	pqsignal(SIGQUIT, SignalHandlerForCrashExit);

	sigdelset(&BlockSig, SIGQUIT);
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);
}

/*
 * Simple signal handler for triggering a configuration reload.
 *
 * Normally, this handler would be used for SIGHUP. The idea is that code
 * which uses it would arrange to check the INTERRUPT_CONFIG_RELOAD interrupt
 * at convenient places inside main loops.
 */
void
SignalHandlerForConfigReload(SIGNAL_ARGS)
{
	RaiseInterrupt(INTERRUPT_CONFIG_RELOAD);
}

/*
 * Simple signal handler for exiting quickly as if due to a crash.
 *
 * Normally, this would be used for handling SIGQUIT.
 */
void
SignalHandlerForCrashExit(SIGNAL_ARGS)
{
	/*
	 * We DO NOT want to run proc_exit() or atexit() callbacks -- we're here
	 * because shared memory may be corrupted, so we don't want to try to
	 * clean up our transaction.  Just nail the windows shut and get out of
	 * town.  The callbacks wouldn't be safe to run from a signal handler,
	 * anyway.
	 *
	 * Note we do _exit(2) not _exit(0).  This is to force the postmaster into
	 * a system reset cycle if someone sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
	 * should ensure the postmaster sees this as a crash, too, but no harm in
	 * being doubly sure.)
	 */
	_exit(2);
}

/*
 * Simple signal handler for triggering a long-running process to shut down
 * and exit.
 *
 * In most processes, this handler is used for SIGTERM, but some processes use
 * other signals.
 */
void
SignalHandlerForShutdownRequest(SIGNAL_ARGS)
{
	RaiseInterrupt(INTERRUPT_TERMINATE);
}

/*
 * Query-cancel signal: abort current transaction at soonest convenient time
 */
void
SignalHandlerForQueryCancel(SIGNAL_ARGS)
{
	RaiseInterrupt(INTERRUPT_QUERY_CANCEL);
}
