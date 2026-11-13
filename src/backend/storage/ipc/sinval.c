/*-------------------------------------------------------------------------
 *
 * sinval.c
 *	  POSTGRES shared cache invalidation communication code.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/sinval.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "ipc/interrupt.h"
#include "miscadmin.h"
#include "storage/sinvaladt.h"
#include "utils/injection_point.h"
#include "utils/inval.h"


uint64		SharedInvalidMessageCounter;

/*
 * SendSharedInvalidMessages
 *	Add shared-cache-invalidation message(s) to the global SI message queue.
 */
void
SendSharedInvalidMessages(const SharedInvalidationMessage *msgs, int n)
{
	SIInsertDataEntries(msgs, n);
}

/*
 * ReceiveSharedInvalidMessages
 *		Process shared-cache-invalidation messages waiting for this backend
 *
 * We guarantee to process all messages that had been queued before the
 * routine was entered.  It is of course possible for more messages to get
 * queued right after our last SIGetDataEntries call.
 *
 * NOTE: it is entirely possible for this routine to be invoked recursively
 * as a consequence of processing inside the invalFunction or resetFunction.
 * Furthermore, such a recursive call must guarantee that all outstanding
 * inval messages have been processed before it exits.  This is the reason
 * for the strange-looking choice to use a statically allocated buffer array
 * and counters; it's so that a recursive call can process messages already
 * sucked out of sinvaladt.c.
 */
void
ReceiveSharedInvalidMessages(void (*invalFunction) (SharedInvalidationMessage *msg),
							 void (*resetFunction) (void))
{
#define MAXINVALMSGS 32
	static SharedInvalidationMessage messages[MAXINVALMSGS];

	/*
	 * We use volatile here to prevent bugs if a compiler doesn't realize that
	 * recursion is a possibility ...
	 */
	static volatile int nextmsg = 0;
	static volatile int nummsgs = 0;
	bool		read_any = false;
	bool		catchup_requested = false;

	/*
	 * Consume the catchup interrupt before reading any messages, so that if a
	 * new catchup interrupt arrives while we're processing, we'll process it
	 * too.
	 */
	if (ConsumeInterrupt(INTERRUPT_SINVAL_CATCHUP))
		catchup_requested = true;

	/* Deal with any messages still pending from an outer recursion */
	while (nextmsg < nummsgs)
	{
		SharedInvalidationMessage msg = messages[nextmsg++];

		SharedInvalidMessageCounter++;
		invalFunction(&msg);
	}

	do
	{
		int			getResult;

		nextmsg = nummsgs = 0;

		/* Try to get some more messages */
		getResult = SIGetDataEntries(messages, MAXINVALMSGS);
		if (getResult != 0)
			read_any = true;

		if (getResult < 0)
		{
			/* got a reset message */
			elog(DEBUG4, "cache state reset");
			SharedInvalidMessageCounter++;
			resetFunction();
			break;				/* nothing more to do */
		}

		/* Process them, being wary that a recursive call might eat some */
		nextmsg = 0;
		nummsgs = getResult;

		while (nextmsg < nummsgs)
		{
			SharedInvalidationMessage msg = messages[nextmsg++];

			SharedInvalidMessageCounter++;
			invalFunction(&msg);
		}

		/*
		 * We only need to loop if the last SIGetDataEntries call (which might
		 * have been within a recursive call) returned a full buffer.
		 */
	} while (nummsgs == MAXINVALMSGS);

	/*
	 * Allow tests to pause here, after we have caught up, but before we pass
	 * on any catchup request.  Only if we read any messages or a reset, so
	 * that this is not reached on every call.
	 */
	if (read_any)
		INJECTION_POINT("sinval-caught-up", NULL);

	/*
	 * We are now caught up.  If we received a catchup signal, call
	 * SICleanupQueue().  This is not so much because we need to flush dead
	 * messages right now, as that we want to pass on the catchup signal to
	 * the next slowest backend.  "Daisy chaining" the catchup signal this way
	 * avoids creating spikes in system load for what should be just a
	 * background maintenance activity.
	 */
	if (catchup_requested)
	{
		elog(DEBUG4, "sinval catchup complete, cleaning queue");
		SICleanupQueue(false, 0);
	}
}

/*
 * ProcessCatchupInterrupt
 *		Process a pending INTERRUPT_SINVAL_CATCHUP interrupt
 *
 * This is not a CHECK_FOR_INTERRUPTS() handler, because most code is not
 * prepared for cache invalidations to be processed at an arbitrary
 * CHECK_FOR_INTERRUPTS(), e.g. in the middle of a transaction or while
 * building a cache entry.  Instead, processes that can sit idle for a long
 * time include INTERRUPT_SINVAL_CATCHUP in their wait mask, and call this
 * explicitly at a point where it's safe.
 *
 * This must be called with interrupts not held off, as catching up can
 * involve catalog access and lock waits, and we want e.g. a termination
 * request to be processed meanwhile.
 */
void
ProcessCatchupInterrupt(void)
{
	Assert(INTERRUPTS_CAN_BE_PROCESSED());

	/*
	 * The caller is expected to only call this function if
	 * INTERRUPT_SINVAL_CATCHUP was set, but they may have already cleared it
	 * before calling.  Re-arm it, because ReceiveSharedInvalidMessages() uses
	 * it as a signal to call SICleanupQueue().
	 *
	 * It would be more straightforward for the caller to refrain from
	 * clearing the interrupt before calling this function, but the convention
	 * with interrupt handlers is that the ProcessInterrupts() clears the
	 * interrupt before calling the handler.  Even though this function is not
	 * registered as interrupt handler, it seems better to stick to that
	 * convention here too.
	 */
	RaiseInterrupt(INTERRUPT_SINVAL_CATCHUP);

	while (InterruptPending(INTERRUPT_SINVAL_CATCHUP))
	{
		/*
		 * What we need to do here is cause ReceiveSharedInvalidMessages() to
		 * run, which will do the necessary work and also clear the interrupt.
		 * (If a new catchup signal arrives after it has cleared the
		 * interrupt, we loop to process that too.)  If we are inside a
		 * transaction we can just call AcceptInvalidationMessages() to do
		 * this.  If we aren't, we start and immediately end a transaction;
		 * the call to AcceptInvalidationMessages() happens down inside
		 * transaction start.
		 *
		 * It is awfully tempting to just call AcceptInvalidationMessages()
		 * without the rest of the xact start/stop overhead, and I think that
		 * would actually work in the normal case; but I am not sure that
		 * things would clean up nicely if we got an error partway through.
		 */
		if (IsTransactionOrTransactionBlock())
		{
			elog(DEBUG4, "ProcessCatchupInterrupt inside transaction");
			AcceptInvalidationMessages();
		}
		else
		{
			elog(DEBUG4, "ProcessCatchupInterrupt outside transaction");
			StartTransactionCommand();
			CommitTransactionCommand();
		}

		/*
		 * If another catchup interrupt arrived while we were processing the
		 * previous one, loop to process the new one too.
		 */
	}
}
