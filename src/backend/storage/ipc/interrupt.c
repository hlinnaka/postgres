/*-------------------------------------------------------------------------
 *
 * interrupt.c
 *	  Interrupt handling routines.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/interrupt.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/interrupt.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/waiteventset.h"
#include "utils/guc.h"
#include "utils/memutils.h"

static pg_atomic_uint32 LocalPendingInterrupts;

pg_atomic_uint32 *MyPendingInterrupts = &LocalPendingInterrupts;

/*
 * Switch to local interrupts.  Other backends can't send interrupts to this
 * one.  Only RaiseInterrupt() can set them, from inside this process.
 */
void
SwitchToLocalInterrupts(void)
{
	if (MyPendingInterrupts == &LocalPendingInterrupts)
		return;

	MyPendingInterrupts = &LocalPendingInterrupts;

	/*
	 * Make sure that SIGALRM handlers that call RaiseInterrupt() are now
	 * seeing the new MyPendingInterrupts destination.
	 */
	pg_memory_barrier();

	/*
	 * Mix in the interrupts that we have received already in our shared
	 * interrupt vector, while atomically clearing it.  Other backends may
	 * continue to set bits in it after this point, but we've atomically
	 * transferred the existing bits to our local vector so we won't get
	 * duplicated interrupts later if we switch backx.
	 */
	pg_atomic_fetch_or_u32(MyPendingInterrupts,
						   pg_atomic_exchange_u32(&MyProc->pendingInterrupts, 0));
}

/*
 * Switch to shared memory interrupts.  Other backends can send interrupts to
 * this one if they know its ProcNumber, and we'll now see any that we missed.
 */
void
SwitchToSharedInterrupts(void)
{
	if (MyPendingInterrupts == &MyProc->pendingInterrupts)
		return;

	MyPendingInterrupts = &MyProc->pendingInterrupts;

	/*
	 * Make sure that SIGALRM handlers that call RaiseInterrupt() are now
	 * seeing the new MyPendingInterrupts destination.
	 */
	pg_memory_barrier();

	/* Mix in any unhandled bits from LocalPendingInterrupts. */
	pg_atomic_fetch_or_u32(MyPendingInterrupts,
						   pg_atomic_exchange_u32(&LocalPendingInterrupts, 0));
}

/*
 * Set an interrupt flag in this backend.
 */
void
RaiseInterrupt(InterruptType reason)
{
	uint32		old_pending;

	old_pending = pg_atomic_fetch_or_u32(MyPendingInterrupts, 1 << reason);

	/*
	 * If the process is currently blocked waiting for an interrupt to arrive,
	 * and the interrupt wasn't already pending, wake it up.
	 */
	if ((old_pending & (1 << reason | 1 << SLEEPING_ON_INTERRUPTS)) == 1 << SLEEPING_ON_INTERRUPTS)
		WakeupMyProc();
}

/*
 * Set an interrupt flag in another backend.
 */
void
SendInterrupt(InterruptType reason, ProcNumber pgprocno)
{
	PGPROC	   *proc;
	uint32		old_pending;

	Assert(pgprocno != INVALID_PROC_NUMBER);
	Assert(pgprocno >= 0);
	Assert(pgprocno < ProcGlobal->allProcCount);

	proc = &ProcGlobal->allProcs[pgprocno];
	old_pending = pg_atomic_fetch_or_u32(&proc->pendingInterrupts, 1 << reason);

	/*
	 * If the process is currently blocked waiting for an interrupt to arrive,
	 * and the interrupt wasn't already pending, wake it up.
	 */
	if ((old_pending & (1 << reason | 1 << SLEEPING_ON_INTERRUPTS)) == 1 << SLEEPING_ON_INTERRUPTS)
		WakeupOtherProc(proc);
}
