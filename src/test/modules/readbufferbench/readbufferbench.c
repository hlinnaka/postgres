/*--------------------------------------------------------------------------
 *
 * readbufferbench.c
 *		Test false positive rate of Bloom filter.
 *
 * Copyright (c) 2018-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/readbufferbench/readbufferbench.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;


PG_FUNCTION_INFO_V1(readbuffer_bench);

Datum
readbuffer_bench(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		nblocks = PG_GETARG_INT32(1);
	uint32		niters = PG_GETARG_UINT32(2);
	Relation	rel;
	Buffer		buf;

	if (PG_NARGS() != 3)
		elog(ERROR, "wrong number of arguments");

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser")));

	rel = relation_open(relid, AccessShareLock);

	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get page from relation \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	if (nblocks < 0)
		nblocks = RelationGetNumberOfBlocksInFork(rel, MAIN_FORKNUM);
	else if (nblocks > RelationGetNumberOfBlocksInFork(rel, MAIN_FORKNUM))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation has only %d blocks",
						nblocks)));

#if 1
	{
		BufferManagerRelation bmr;

		InitBMRForRel(&bmr, rel, MAIN_FORKNUM, NULL);
		for (int i = 0; i < niters; i++)
		{
			for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
			{
				buf = ReadBufferBMR(&bmr, blkno, RBM_NORMAL);
				ReleaseBuffer(buf);
			}
		}
	}
#else
	for (int i = 0; i < niters; i++)
	{
		for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
		{
			buf = ReadBuffer(rel, blkno);
			ReleaseBuffer(buf);
		}
	}
#endif

	relation_close(rel, AccessShareLock);

	PG_RETURN_VOID();
}
