/*--------------------------------------------------------------------------
 *
 * multixid_wraparound.c
 *		Utilities for testing multixids
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/xid_wraparound/multixid_wraparound.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/xid8.h"

static int mxactMemberComparator(const void *arg1, const void *arg2);

/*
 * Consume the specified number of multi-XIDs, with specified number of
 * members each.
 */
PG_FUNCTION_INFO_V1(consume_multixids);
Datum
consume_multixids(PG_FUNCTION_ARGS)
{
	int64		nmultis = PG_GETARG_INT64(0);
	int32		nmembers = PG_GETARG_INT32(1);
	MultiXactMember *members;
	MultiXactId	lastmxid = InvalidMultiXactId;

	if (nmultis < 0)
		elog(ERROR, "invalid nxids argument: %" PRId64, nmultis);
	if (nmembers < 1)
		elog(ERROR, "invalid nmembers argument: %d", nmembers);

	/*
	 * We consume XIDs by calling GetNewTransactionId(true), which marks the
	 * consumed XIDs as subtransactions of the current top-level transaction.
	 * For that to work, this transaction must have a top-level XID.
	 *
	 * GetNewTransactionId registers them in the subxid cache in PGPROC, until
	 * the cache overflows, but beyond that, we don't keep track of the
	 * consumed XIDs.
	 */
	(void) GetTopTransactionId();

	members = palloc((nmultis + nmembers) * sizeof(MultiXactMember));
	for (int32 i = 0; i < nmultis + nmembers; i++)
	{
		FullTransactionId xid;

		xid = GetNewTransactionId(true);
		members[i].xid = XidFromFullTransactionId(xid);
		members[i].status = MultiXactStatusForKeyShare;
	}
	/*
	 * pre-sort the array like mXactCacheGetBySet does, so that the qsort call
	 * in mXactCacheGetBySet() is cheaper.
	 */
	qsort(members, nmultis + nmembers, sizeof(MultiXactMember), mxactMemberComparator);

	for (int64 i = 0; i < nmultis; i++)
	{
		lastmxid = MultiXactIdCreateFromMembers(nmembers, &members[i]);
		CHECK_FOR_INTERRUPTS();
	}

	pfree(members);

	PG_RETURN_TRANSACTIONID(lastmxid);
}

/* copied from multixact.c */
static int
mxactMemberComparator(const void *arg1, const void *arg2)
{
	MultiXactMember member1 = *(const MultiXactMember *) arg1;
	MultiXactMember member2 = *(const MultiXactMember *) arg2;

	if (member1.xid > member2.xid)
		return 1;
	if (member1.xid < member2.xid)
		return -1;
	if (member1.status > member2.status)
		return 1;
	if (member1.status < member2.status)
		return -1;
	return 0;
}
