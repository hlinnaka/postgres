/*-----------------------------------------------------------------------------
 *
 * csn_log.c
 *		Track commit sequence numbers of finished transactions
 *
 * This module provides SLRU to store CSN for each transaction.  This
 * mapping need to be kept only for xid's greater then oldestXid, but
 * that can require arbitrary large amounts of memory in case of long-lived
 * transactions.  Because of same lifetime and persistancy requirements
 * this module is quite similar to subtrans.c
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/csn_log.c
 *
 *-----------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/csn_log.h"
#include "access/slru.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "utils/snapmgr.h"

/*
 * Defines for CSNLog page sizes.  A page is the same BLCKSZ as is used
 * everywhere else in Postgres.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * CSNLog page numbering also wraps around at
 * 0xFFFFFFFF/CSN_LOG_XACTS_PER_PAGE, and CSNLog segment numbering at
 * 0xFFFFFFFF/CLOG_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT.  We need take no
 * explicit notice of that fact in this module, except when comparing segment
 * and page numbers in TruncateCSNLog (see CSNLogPagePrecedes).
 */

/* We store the commit CSN for each xid */
#define CSN_LOG_XACTS_PER_PAGE (BLCKSZ / sizeof(XLogRecPtr))

#define TransactionIdToPage(xid)	((xid) / (TransactionId) CSN_LOG_XACTS_PER_PAGE)
#define TransactionIdToPgIndex(xid) ((xid) % (TransactionId) CSN_LOG_XACTS_PER_PAGE)

#define PgIndexToTransactionId(pageno, idx) (CSN_LOG_XACTS_PER_PAGE * (pageno) + idx)



/*
 * Link to shared-memory data structures for CLOG control
 */
static SlruCtlData CSNLogCtlData;
#define CsnlogCtl (&CSNLogCtlData)

static int	ZeroCSNLogPage(int pageno);
static bool CSNLogPagePrecedes(int64 page1, int64 page2);
static void CSNLogSetPageStatus(TransactionId xid, int nsubxids,
									  TransactionId *subxids,
									  XLogRecPtr csn, int pageno);
static void CSNLogSetCSNInSlot(TransactionId xid, XLogRecPtr csn,
									  int slotno);


/*
 * CSNLogSetCSN
 *
 * Record XidCSN of transaction and its subtransaction tree.
 *
 * xid is a single xid to set status for. This will typically be the top level
 * transactionid for a top level commit or abort. It can also be a
 * subtransaction when we record transaction aborts.
 *
 * subxids is an array of xids of length nsubxids, representing subtransactions
 * in the tree of xid. In various cases nsubxids may be zero.
 *
 * csn is the commit sequence number of the transaction. It should be
 * AbortedCSN for abort cases.
 */
void
CSNLogSetCSN(TransactionId xid, int nsubxids,
					 TransactionId *subxids, XLogRecPtr csn)
{
	int			pageno;
	int			i = 0;
	int			offset = 0;

	Assert(TransactionIdIsValid(xid));

	pageno = TransactionIdToPage(xid);		/* get page of parent */
	for (;;)
	{
		int			num_on_page = 0;

		while (i < nsubxids && TransactionIdToPage(subxids[i]) == pageno)
		{
			num_on_page++;
			i++;
		}

		CSNLogSetPageStatus(xid,
							num_on_page, subxids + offset,
							csn, pageno);
		if (i >= nsubxids)
			break;

		offset = i;
		pageno = TransactionIdToPage(subxids[offset]);
		xid = InvalidTransactionId;
	}
}

/*
 * Record the final state of transaction entries in the csn log for
 * all entries on a single page.  Atomic only on this page.
 *
 * Otherwise API is same as TransactionIdSetTreeStatus()
 */
static void
CSNLogSetPageStatus(TransactionId xid, int nsubxids,
						   TransactionId *subxids,
						   XLogRecPtr csn, int pageno)
{
	int			slotno;
	int			i;
	LWLock	   *lock;

	lock = SimpleLruGetBankLock(CsnlogCtl, pageno);
	LWLockAcquire(lock, LW_EXCLUSIVE);

	slotno = SimpleLruReadPage(CsnlogCtl, pageno, true, xid);

	/* Subtransactions first, if needed ... */
	for (i = 0; i < nsubxids; i++)
	{
		Assert(CsnlogCtl->shared->page_number[slotno] == TransactionIdToPage(subxids[i]));
		CSNLogSetCSNInSlot(subxids[i],	csn, slotno);
	}

	/* ... then the main transaction */
	if (TransactionIdIsValid(xid))
		CSNLogSetCSNInSlot(xid, csn, slotno);

	CsnlogCtl->shared->page_dirty[slotno] = true;

	LWLockRelease(lock);
}

/*
 * Sets the commit status of a single transaction.
 */
static void
CSNLogSetCSNInSlot(TransactionId xid, XLogRecPtr csn, int slotno)
{
	int			entryno = TransactionIdToPgIndex(xid);
	XLogRecPtr *ptr;

	ptr = (XLogRecPtr *) (CsnlogCtl->shared->page_buffer[slotno] + entryno * sizeof(XLogRecPtr));

	*ptr = csn;
}

/*
 * Interrogate the state of a transaction in the log.
 *
 * NB: this is a low-level routine and is NOT the preferred entry point
 * for most uses; TransactionIdGetXidCSN() in csn_snapshot.c is the
 * intended caller.
 */
XLogRecPtr
CSNLogGetCSNByXid(TransactionId xid)
{
	int			pageno = TransactionIdToPage(xid);
	int			entryno = TransactionIdToPgIndex(xid);
	int			slotno;
	XLogRecPtr *ptr;
	XLogRecPtr	xid_csn;

	Assert(TransactionIdIsValid(xid));

	/* Can't ask about stuff that might not be around anymore */
	Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

	/* lock is acquired by SimpleLruReadPage_ReadOnly */

	slotno = SimpleLruReadPage_ReadOnly(CsnlogCtl, pageno, xid);
	ptr = (XLogRecPtr *) (CsnlogCtl->shared->page_buffer[slotno] + entryno * sizeof(XLogRecPtr));
	xid_csn = *ptr;

	LWLockRelease(SimpleLruGetBankLock(CsnlogCtl, pageno));

	return xid_csn;
}

/*
 * Number of shared CSNLog buffers.
 */
static Size
CSNLogShmemBuffers(void)
{
	return Min(32, Max(16, NBuffers / 512));
}

/*
 * Reserve shared memory for CsnlogCtl.
 */
Size
CSNLogShmemSize(void)
{
	// FIXME: skip if not InHotStandby?
	return SimpleLruShmemSize(CSNLogShmemBuffers(), 0);
}

/*
 * Initialization of shared memory for CSNLog.
 */
void
CSNLogShmemInit(void)
{
	CsnlogCtl->PagePrecedes = CSNLogPagePrecedes;
	SimpleLruInit(CsnlogCtl, "CSNLog Ctl", CSNLogShmemBuffers(), 0,
				  "pg_csn", LWTRANCHE_CSN_LOG_BUFFER,
				  LWTRANCHE_CSN_LOG_SLRU, SYNC_HANDLER_NONE, false);
	//SlruPagePrecedesUnitTests(CsnlogCtl, SUBTRANS_XACTS_PER_PAGE);
}

/*
 * This func must be called ONCE on system install.  It creates the initial
 * CSNLog segment.  The pg_csn directory is assumed to have been
 * created by initdb, and CSNLogShmemInit must have been called already.
 *
 * Note: it's not really necessary to create the initial segment now,
 * since slru.c would create it on first write anyway.  But we may as well
 * do it to be sure the directory is set up correctly.
 */
void
BootStrapCSNLog(void)
{
	int			slotno;
	LWLock	   *lock;

	lock = SimpleLruGetBankLock(CsnlogCtl, 0);
	LWLockAcquire(lock, LW_EXCLUSIVE);

	/* Create and zero the first page of the commit log */
	slotno = ZeroCSNLogPage(0);

	/* Make sure it's written out */
	SimpleLruWritePage(CsnlogCtl, slotno);
	Assert(!CsnlogCtl->shared->page_dirty[slotno]);

	LWLockRelease(lock);
}

/*
 * Initialize (or reinitialize) a page of CSNLog to zeroes.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ZeroCSNLogPage(int pageno)
{
	return SimpleLruZeroPage(CsnlogCtl, pageno);
}

/*
 * Initialize a page of CSNLog based on pg_xact.
 *
 */
static void
InitCSNLogPage(int pageno, TransactionId *xid, TransactionId nextXid, XLogRecPtr csn)
{
	XLogRecPtr	dummy;
	int			slotno;

	slotno = ZeroCSNLogPage(pageno);

	while (*xid < nextXid && TransactionIdToPage(*xid) == pageno)
	{
		XidStatus status = TransactionIdGetStatus(*xid, &dummy);
		if (status == TRANSACTION_STATUS_COMMITTED ||
			status == TRANSACTION_STATUS_ABORTED)
			CSNLogSetCSNInSlot(*xid, csn, slotno);

		TransactionIdAdvance(*xid);
	}
	SimpleLruZeroPage(CsnlogCtl, pageno);
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup,
 * after StartupXLOG has initialized ShmemVariableCache->nextXid.
 *
 * oldestActiveXID is the oldest XID of any prepared transaction, or nextXid
 * if there are none.
 *
 * All transactions that have already completed are marked with 'csn'.
 */
void
StartupCSNLog(TransactionId oldestActiveXID, XLogRecPtr csn)
{
	TransactionId xid;
	FullTransactionId nextXid;
	int			startPage;
	int			endPage;
	LWLock	   *prevlock = NULL;
	LWLock	   *lock;

	/*
	 * Since we don't expect pg_csn to be valid across crashes, we
	 * initialize the currently-active page(s) to zeroes during startup.
	 * Whenever we advance into a new page, ExtendCSNLog will likewise
	 * zero the new page without regard to whatever was previously on disk.
	 */
	startPage = TransactionIdToPage(oldestActiveXID);
	nextXid = TransamVariables->nextXid;
	endPage = TransactionIdToPage(XidFromFullTransactionId(nextXid));

	Assert(TransactionIdIsValid(oldestActiveXID));
	Assert(FullTransactionIdIsValid(nextXid));

	xid = oldestActiveXID;
	for (;;)
	{
		lock = SimpleLruGetBankLock(CsnlogCtl, startPage);
		if (prevlock != lock)
		{
			if (prevlock)
				LWLockRelease(prevlock);
			LWLockAcquire(lock, LW_EXCLUSIVE);
			prevlock = lock;
		}

		InitCSNLogPage(startPage, &xid, XidFromFullTransactionId(nextXid), csn);
		if (startPage == endPage)
			break;

		startPage++;
		/* must account for wraparound */
		if (startPage > TransactionIdToPage(MaxTransactionId))
			startPage = 0;
	}

	LWLockRelease(lock);
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownCSNLog(void)
{
	/*
	 * Flush dirty CSNLog pages to disk.
	 *
	 * This is not actually necessary from a correctness point of view. We do
	 * it merely as a debugging aid.
	 */
	TRACE_POSTGRESQL_CSNLOG_CHECKPOINT_START(false);
	SimpleLruWriteAll(CsnlogCtl, false);
	TRACE_POSTGRESQL_CSNLOG_CHECKPOINT_DONE(false);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointCSNLog(void)
{
	/*
	 * Flush dirty CSNLog pages to disk.
	 *
	 * This is not actually necessary from a correctness point of view. We do
	 * it merely to improve the odds that writing of dirty pages is done by
	 * the checkpoint process and not by backends.
	 */
	TRACE_POSTGRESQL_CSNLOG_CHECKPOINT_START(true);
	SimpleLruWriteAll(CsnlogCtl, true);
	TRACE_POSTGRESQL_CSNLOG_CHECKPOINT_DONE(true);
}

/*
 * Make sure that CSNLog has room for a newly-allocated XID.
 *
 * NB: this is called while holding XidGenLock.  We want it to be very fast
 * most of the time; even when it's not so fast, no actual I/O need happen
 * unless we're forced to write out a dirty clog or xlog page to make room
 * in shared memory.
 */
void
ExtendCSNLog(TransactionId newestXact)
{
	int64		pageno;
	LWLock	   *lock;

	/*
	 * No work except at first XID of a page.  But beware: just after
	 * wraparound, the first XID of page zero is FirstNormalTransactionId.
	 */
	if (TransactionIdToPgIndex(newestXact) != 0 &&
		!TransactionIdEquals(newestXact, FirstNormalTransactionId))
		return;

	pageno = TransactionIdToPage(newestXact);

	lock = SimpleLruGetBankLock(CsnlogCtl, pageno);

	LWLockAcquire(lock, LW_EXCLUSIVE);

	/* Zero the page and make an XLOG entry about it */
	ZeroCSNLogPage(pageno);

	LWLockRelease(lock);
}

/*
 * Remove all CSNLog segments before the one holding the passed
 * transaction ID.
 *
 * This is normally called during checkpoint, with oldestXact being the
 * oldest TransactionXmin of any running transaction.
 */
void
TruncateCSNLog(TransactionId oldestXact)
{
	int			cutoffPage;

	/*
	 * The cutoff point is the start of the segment containing oldestXact. We
	 * pass the *page* containing oldestXact to SimpleLruTruncate. We step
	 * back one transaction to avoid passing a cutoff page that hasn't been
	 * created yet in the rare case that oldestXact would be the first item on
	 * a page and oldestXact == next XID.  In that case, if we didn't subtract
	 * one, we'd trigger SimpleLruTruncate's wraparound detection.
	 */
	TransactionIdRetreat(oldestXact);
	cutoffPage = TransactionIdToPage(oldestXact);

	SimpleLruTruncate(CsnlogCtl, cutoffPage);
}

/*
 * Decide which of two CSNLog page numbers is "older" for truncation
 * purposes.
 *
 * We need to use comparison of TransactionIds here in order to do the right
 * thing with wraparound XID arithmetic.  However, if we are asked about
 * page number zero, we don't want to hand InvalidTransactionId to
 * TransactionIdPrecedes: it'll get weird about permanent xact IDs.  So,
 * offset both xids by FirstNormalTransactionId to avoid that.
 */
static bool
CSNLogPagePrecedes(int64 page1, int64 page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * CSN_LOG_XACTS_PER_PAGE;
	xid1 += FirstNormalTransactionId;
	xid2 = ((TransactionId) page2) * CSN_LOG_XACTS_PER_PAGE;
	xid2 += FirstNormalTransactionId;

	return TransactionIdPrecedes(xid1, xid2);
}
