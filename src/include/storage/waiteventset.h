/*-------------------------------------------------------------------------
 *
 * waiteventset.h
 *	  ppoll() / pselect() like interface for waiting for events
 *
 * This is a reliable replacement for the common pattern of using pg_usleep()
 * or select() to wait until a signal arrives, where the signal handler raises
 * an interrupt (see storage/interrupt.h). Because on some platforms an
 * incoming signal doesn't interrupt sleep, and even on platforms where it
 * does there is a race condition if the signal arrives just before entering
 * the sleep, the common pattern must periodically wake up and poll the flag
 * variable. The pselect() system call was invented to solve this problem, but
 * it is not portable enough. WaitEventSets and Interrupts are designed to
 * overcome these limitations, allowing you to sleep without polling and
 * ensuring quick response to signals from other processes.
 *
 * WaitInterrupt includes a provision for timeouts (which should be avoided
 * when possible, as they incur extra overhead) and a provision for postmaster
 * child processes to wake up immediately on postmaster death.  See
 * interrupt.c for detailed specifications for the exported functions.
 *
 * On some platforms, signals will not interrupt the wait primitive by
 * themselves.  Therefore, it is critical that any signal handler that is
 * meant to terminate a WaitInterrupt wait calls RaiseInterrupt.
 *
 * WaitEventSets allow to wait for interrupts being set and additional events -
 * postmaster dying and socket readiness of several sockets currently - at the
 * same time.  On many platforms using a long lived event set is more
 * efficient than using WaitInterrupt or WaitInterruptOrSocket.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/waiteventset.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WAITEVENTSET_H
#define WAITEVENTSET_H

#include <signal.h>

#include "storage/procnumber.h"
#include "utils/resowner.h"

/*
 * Bitmasks for events that may wake-up WaitLatch(), WaitLatchOrSocket(), or
 * WaitEventSetWait().
 */
#define WL_INTERRUPT		 (1 << 0)
#define WL_SOCKET_READABLE	 (1 << 1)
#define WL_SOCKET_WRITEABLE  (1 << 2)
#define WL_TIMEOUT			 (1 << 3)	/* not for WaitEventSetWait() */
#define WL_POSTMASTER_DEATH  (1 << 4)
#define WL_EXIT_ON_PM_DEATH	 (1 << 5)
#ifdef WIN32
#define WL_SOCKET_CONNECTED  (1 << 6)
#else
/* avoid having to deal with case on platforms not requiring it */
#define WL_SOCKET_CONNECTED  WL_SOCKET_WRITEABLE
#endif
#define WL_SOCKET_CLOSED 	 (1 << 7)
#ifdef WIN32
#define WL_SOCKET_ACCEPT	 (1 << 8)
#else
/* avoid having to deal with case on platforms not requiring it */
#define WL_SOCKET_ACCEPT	WL_SOCKET_READABLE
#endif
#define WL_SOCKET_MASK		(WL_SOCKET_READABLE | \
							 WL_SOCKET_WRITEABLE | \
							 WL_SOCKET_CONNECTED | \
							 WL_SOCKET_ACCEPT | \
							 WL_SOCKET_CLOSED)

typedef struct WaitEvent
{
	int			pos;			/* position in the event data structure */
	uint32		events;			/* triggered events */
	pgsocket	fd;				/* socket fd associated with event */
	void	   *user_data;		/* pointer provided in AddWaitEventToSet */
#ifdef WIN32
	bool		reset;			/* Is reset of the event required? */
#endif
} WaitEvent;

/* forward declaration to avoid exposing latch.c implementation details */
typedef struct WaitEventSet WaitEventSet;

struct PGPROC;

/*
 * prototypes for functions in waiteventset.c
 */
extern void InitializeWaitEventSupport(void);
extern void ShutdownWaitEventSupport(void);
#ifdef WIN32
extern HANDLE CreateSharedWakeupHandle(void);
#endif

extern WaitEventSet *CreateWaitEventSet(ResourceOwner resowner, int nevents);
extern void FreeWaitEventSet(WaitEventSet *set);
extern void FreeWaitEventSetAfterFork(WaitEventSet *set);
extern int	AddWaitEventToSet(WaitEventSet *set, uint32 events, pgsocket fd,
							  uint32 interruptMask, void *user_data);
extern void ModifyWaitEvent(WaitEventSet *set, int pos, uint32 events, uint32 interruptMask);

extern int	WaitEventSetWait(WaitEventSet *set, long timeout,
							 WaitEvent *occurred_events, int nevents,
							 uint32 wait_event_info);
extern void InitializeInterruptWaitSet(void);
extern int	GetNumRegisteredWaitEvents(WaitEventSet *set);
extern bool WaitEventSetCanReportClosed(void);

/* low level functions used to implement SendInterrupt */
extern void WakeupMyProc(void);
extern void WakeupOtherProc(struct PGPROC *proc);

#endif							/* WAITEVENTSET_H */
