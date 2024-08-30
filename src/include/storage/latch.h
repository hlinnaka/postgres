/*-------------------------------------------------------------------------
 *
 * latch.h
 *	  Backwards-compatibility macros for the old Latch interface
 *
 * Latches were an inter-process signalling mechanism that was replaced
 * in PostgreSQL verson 18 with Interrupts, see "storage/interrupt.h".
 * This file contains macros that map old Latch calls to the new interface.
 * The mapping is not perfect, it only covers the basic usage of setting
 * and waiting for the current process's own latch (MyLatch), which is
 * mapped to raising or waiting for INTERRUPT_GENERAL_WAKUP.
 *
 * The WaitEventSet functions that used to be here are now in
 * "storage/waitevents.h".
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/latch.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef LATCH_H
#define LATCH_H

#include "storage/interrupt.h"

#define WL_LATCH_SET WL_INTERRUPT

/*
 * These compatibility macros only support operating on MyLatch. It used to
 * be a pointer to a Latch struct, but now it's just a dummy int variable.
 */

#define MyLatch ((void *) 0xAAAA)

static inline void
SetLatch(void *dummy)
{
	Assert(dummy == MyLatch);
	RaiseInterrupt(INTERRUPT_GENERAL_WAKEUP);
}

static inline void
ResetLatch(void *dummy)
{
	Assert(dummy == MyLatch);
	ClearInterrupt(INTERRUPT_GENERAL_WAKEUP);
}

static inline int
WaitLatch(void *dummy, int wakeEvents, long timeout, uint32 wait_event_info)
{
	uint32		interrupt_mask = 0;

	Assert(dummy == MyLatch || dummy == NULL);

	if ((wakeEvents & WL_LATCH_SET) && dummy == MyLatch)
		interrupt_mask = 1 << INTERRUPT_GENERAL_WAKEUP;
	return WaitInterrupt(interrupt_mask, wakeEvents,
						 timeout, wait_event_info);
}

static inline int
WaitLatchOrSocket(void *dummy, int wakeEvents, pgsocket sock, long timeout,
				  uint32 wait_event_info)
{
	uint32		interrupt_mask = 0;

	Assert(dummy == MyLatch || dummy == NULL);

	if ((wakeEvents & WL_LATCH_SET) && dummy == MyLatch)
		interrupt_mask = 1 << INTERRUPT_GENERAL_WAKEUP;
	return WaitInterruptOrSocket(interrupt_mask, wakeEvents,
								 sock, timeout, wait_event_info);
}

#endif
