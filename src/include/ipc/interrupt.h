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

#include "ipc/standard_interrupts.h"
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
 * PendingInterrupts is used to receive and wait for interrupts.  It contains
 * a bitmask of interrupts pending for the process, and another bitmask of
 * interrupts we're currently interested in.
 *
 * We support up to 64 different interrupts.  That way, an interrupt mask can
 * be conveniently stored as one 64-bit atomic integer, on systems with 64-bit
 * atomics.  On other systems, it's split into two 32-bit atomic fields, which
 * is good enough because we don't rely on atomicity between different
 * interrupt bits.  (Note that the 64-bit atomics simulation relies on
 * spinlocks, which creates a deadlock risk when used from signal handlers, so
 * we cannot rely on the simulated 64-bit atomics.)
 *
 * Attention mechanism
 * -------------------
 *
 * The 'attention_mask' field lets a backend advertise which interrupts it is
 * currently interested in.  When a backend is sleeping, waiting for an
 * interrupt to arrive, it sets the bits for the waited-for interrupts in
 * 'attention_mask'.  At other times, the 'attention_mask' equals
 * EnabledInterruptsMask, i.e. the interrupts that can be processed by a
 * CHECK_FOR_INTERRUPTS().
 *
 * When a backend sets the interrupt bit of another backend (or the same
 * backend), it also checks if that interrupt is in the target's
 * 'attention_mask'.  If so, it sets the ATTENTION flag.  Furthermore, if the
 * target backend is currently sleeping, i.e. if the SLEEPING flag is set, it
 * also wakes it up.
 *
 * When not sleeping, the ATTENTION flag is used as a quick check in
 * CHECK_FOR_INTERRUPTS() for whether any interrupts need to be processed.
 * Checking a single flag requires fewer instructions than checking the
 * interrupt bits against EnabledInterruptsMask; the attention mechanism
 * shifts that work to the sending backend.
 *
 * There are race conditions in how the ATTENTION flag is set.  If a backend
 * clears a bit from its 'attention_mask', and another backend is concurrently
 * sending that interrupt, it's possible that the ATTENTION flag gets set or
 * the process is woken up after the 'attention_mask' has already been
 * cleared.  Because of that, the system needs to tolerate spuriously set
 * ATTENTION flag and wakeups.  The operations are ordered so that the
 * opposite is not possible: if you set a bit in the 'attention_mask' and then
 * check that the bit is not set in the 'interrupts' mask, you are guaranteed
 * to receive the attention flag or a wakeup if the interrupt is set later.
 */
typedef struct PendingInterrupts
{
	pg_atomic_uint32 flags;		/* PI_FLAG_* */

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	pg_atomic_uint64 pending_mask;	/* pending interrupts */
	pg_atomic_uint64 attention_mask;	/* interrupts that set the ATTENTION
										 * flag */
#else
	pg_atomic_uint32 pending_mask_lo;
	pg_atomic_uint32 pending_mask_hi;
	pg_atomic_uint32 attention_mask_lo;
	pg_atomic_uint32 attention_mask_hi;
#endif
} PendingInterrupts;

#define PI_FLAG_ATTENTION		0x01
#define PI_FLAG_SLEEPING		0x02

/*
 * Interrupt vector currently in use for this process.  Most of the time this
 * points to MyProc->pendingInterrupts, but in processes that have no PGPROC
 * entry (yet), it points to a process-private variable, so that interrupts
 * can nevertheless be used from signal handlers in the same process.
 */
extern PGDLLIMPORT session_local PendingInterrupts *MyPendingInterrupts;

/*
 * Test if an interrupt is pending
 *
 * If 'interruptMask' has multiple bits set, returns true if any of them are
 * pending.
 */
static inline bool
InterruptPending(InterruptMask interruptMask)
{
	/*
	 * Note that there is no memory barrier here, because we want this to be
	 * as cheap as possible.  That means that if the interrupt is concurrently
	 * set by another process, we might miss it.  That should be OK, because
	 * the next WaitInterrupt() or equivalent call acts as a synchronization
	 * barrier; we will see the updated value before sleeping.
	 */
	uint64		pending;

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	pending = pg_atomic_read_u64(&MyPendingInterrupts->pending_mask);
#else
	pending = (uint64) pg_atomic_read_u32(&MyPendingInterrupts->pending_mask_lo);
	pending |= (uint64) pg_atomic_read_u32(&MyPendingInterrupts->pending_mask_hi) << 32;
#endif

	return (pending & interruptMask) != 0;
}

/*
 * Clear an interrupt flag (or flags).
 *
 * Note that this does not clear the ATTENTION flag, so if it was already set,
 * the next CHECK_FOR_INTERRUPTS() will make an unnecessary but harmless
 * ProcessInterrupts() call.
 */
static inline void
ClearInterrupt(InterruptMask interruptMask)
{
	uint64		mask = ~interruptMask;

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
	(void) pg_atomic_fetch_and_u64(&MyPendingInterrupts->pending_mask, mask);
#else
	(void) pg_atomic_fetch_and_u32(&MyPendingInterrupts->pending_mask_lo, (uint32) mask);
	(void) pg_atomic_fetch_and_u32(&MyPendingInterrupts->pending_mask_hi, (uint32) (mask >> 32));
#endif
}

/*
 * Test and clear an interrupt flag (or flags).
 */
static inline bool
ConsumeInterrupt(InterruptMask interruptMask)
{
	if (unlikely(InterruptPending(interruptMask)))
	{
		ClearInterrupt(interruptMask);
		return true;
	}
	else
		return false;
}

extern void RaiseInterrupt(InterruptMask interruptMask);
extern void SendInterrupt(InterruptMask interruptMask, ProcNumber pgprocno);
extern void SendInterruptWithPid(InterruptMask interruptMask, ProcNumber pgprocno, pid_t pid);
extern int	WaitInterrupt(InterruptMask interruptMask, int wakeEvents, long timeout,
						  uint32 wait_event_info);
extern int	WaitInterruptOrSocket(InterruptMask interruptMask, int wakeEvents, pgsocket sock,
								  long timeout, uint32 wait_event_info);
extern void SwitchToLocalInterrupts(void);
extern void SwitchToSharedInterrupts(void);
extern void InitializeInterruptWaitSet(void);
extern void InitializeInterruptSupport(void);


/*****************************************************************************
 *		CHECK_FOR_INTERRUPTS() and friends
 *****************************************************************************/

/* Interrupts currently enabled for CHECK_FOR_INTERRUPTS() processing */
extern PGDLLIMPORT session_local InterruptMask EnabledInterruptsMask;

/*
 * Pointer to MyPendingInterrupts->flags, except when interrupt holdoff or a
 * critical section prevents interrupts processing, in which case this points
 * to a dummy all-zeros variable (ZeroPendingInterruptsFlags) instead.  This
 * allows CHECK_FOR_INTERRUPTS() to follow just this one pointer, and not have
 * to check the holdoff counts separately.
 */
extern PGDLLIMPORT session_local pg_atomic_uint32 *MyPendingInterruptsFlags;

/*
 * Check whether any enabled interrupt is pending, without trying to service
 * it immediately.  This is can be used in a HOLD_INTERRUPTS() block to check
 * if the HOLD_INTERRUPTS() is delaying the interrupt processing.
 */
#ifndef WIN32
#define INTERRUPTS_PENDING_CONDITION() \
	(unlikely(InterruptPending(EnabledInterruptsMask)))
#else
#define INTERRUPTS_PENDING_CONDITION() \
	(unlikely(UNBLOCKED_SIGNAL_QUEUE()) ? \
	 pgwin32_dispatch_queued_signals() : (void) 0,	\
	 unlikely(InterruptPending(EnabledInterruptsMask)))
#endif

/*
 * Can interrupts be processed in the current state, i.e. are the interrupts
 * not prevented by the HOLD_INTERRUPTS() or a critical section critical
 * section?
 */
#define INTERRUPTS_CAN_BE_PROCESSED() \
	(InterruptHoldoffCount == 0 && CritSectionCount == 0)

/*
 * Interrupts that would be processed by CHECK_FOR_INTERRUPTS().  This is
 * equal to EnabledInterruptsMask, except when interrupts are held off by
 * HOLD/RESUME_INTERRUPTS() or a critical section.
 */
#define CheckForInterruptsMask \
	(INTERRUPTS_CAN_BE_PROCESSED() ? EnabledInterruptsMask : 0)

/*
 * Service an interrupt, if one is pending and it's safe to service it now.
 *
 * NB: This is called from all over the codebase, and in fairly tight loops,
 * so this needs to be very short and fast when there is no work to do!
 */
#define CHECK_FOR_INTERRUPTS()					\
do { \
	if (unlikely(pg_atomic_read_u32(MyPendingInterruptsFlags) != 0)) \
		ProcessInterrupts();											\
} while(0)

typedef void (*pg_interrupt_handler_t) (void);
extern void SetInterruptHandler(InterruptMask interruptMask, pg_interrupt_handler_t handler);

extern void EnableInterrupt(InterruptMask interruptMask);
extern void DisableInterrupt(InterruptMask interruptMask);

extern void ProcessInterrupts(void);
extern void SetInterruptAttentionMask(InterruptMask mask);

/*****************************************************************************
 *		Critical section and interrupt holdoff mechanism
 *****************************************************************************/

extern PGDLLIMPORT session_local uint32 InterruptHoldoffCount;
extern PGDLLIMPORT session_local uint32 CritSectionCount;

extern PGDLLIMPORT const pg_atomic_uint32 ZeroPendingInterruptsFlags;

static inline void
HOLD_INTERRUPTS(void)
{
	InterruptHoldoffCount++;

	/*
	 * Disable CHECK_FOR_INTERRUPTS() by pointing MyPendingInterruptsFlags to
	 * an all-zeros constant.
	 */
	MyPendingInterruptsFlags = unconstify(pg_atomic_uint32 *, &ZeroPendingInterruptsFlags);
}

static inline void
RESUME_INTERRUPTS(void)
{
	Assert(InterruptHoldoffCount > 0);
	InterruptHoldoffCount--;
	if (InterruptHoldoffCount == 0 && CritSectionCount == 0)
		MyPendingInterruptsFlags = &MyPendingInterrupts->flags;
}

static inline void
START_CRIT_SECTION(void)
{
	CritSectionCount++;
	MyPendingInterruptsFlags = unconstify(pg_atomic_uint32 *, &ZeroPendingInterruptsFlags);
}

static inline void
END_CRIT_SECTION(void)
{
	Assert(CritSectionCount > 0);
	CritSectionCount--;
	if (InterruptHoldoffCount == 0 && CritSectionCount == 0)
		MyPendingInterruptsFlags = &MyPendingInterrupts->flags;
}

extern void ResetInterruptHoldoffCounts(uint32 new_holdoff_count, uint32 new_crit_section_count);

#endif							/* IPC_INTERRUPT_H */
