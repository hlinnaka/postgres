/*-------------------------------------------------------------------------
 *
 * signal_handlers.h
 *	  Standard signal handling routines.
 *
 * Responses to signals are fairly varied and many types of backends have
 * their own implementations, but we provide a few generic things here to
 * facilitate code reuse.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/ipc/signal_handlers.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef SIGNAL_HANDLERS_H
#define SIGNAL_HANDLERS_H

#include <signal.h>

extern PGDLLIMPORT volatile sig_atomic_t ConfigReloadPending;
extern PGDLLIMPORT volatile sig_atomic_t ShutdownRequestPending;

extern void ProcessMainLoopInterrupts(void);
extern void SignalHandlerForConfigReload(SIGNAL_ARGS);
extern void SignalHandlerForCrashExit(SIGNAL_ARGS);
extern void SignalHandlerForShutdownRequest(SIGNAL_ARGS);

#endif
