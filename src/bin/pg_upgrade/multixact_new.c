/*
 * multixact_new.c
 *
 * Functions to write multixact SLRUs in the current format with 64-bit
 * MultiXactOffsets, used since PostgreSQL version 19.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/multixact_new.c
 */

#include "postgres_fe.h"

#include "access/multixact_internal.h"
#include "multixact_new.h"

MultiXactWriter *
AllocMultiXactWrite(const char *pgdata, MultiXactId firstMulti,
					MultiXactOffset firstOffset)
{
	MultiXactWriter *state = pg_malloc(sizeof(*state));
	char		dir[MAXPGPATH] = {0};

	pg_sprintf(dir, "%s/pg_multixact/offsets", pgdata);
	state->offset = AllocSlruWrite(dir, false);
	SlruWriteSwitchPage(state->offset, MultiXactIdToOffsetPage(firstMulti));

	pg_sprintf(dir, "%s/pg_multixact/members", pgdata);
	state->members = AllocSlruWrite(dir, true /* use long segment names */ );
	SlruWriteSwitchPage(state->members, MXOffsetToMemberPage(firstOffset));

	return state;
}

/*
 * Write a new multixact with members.
 *
 * Simplified version of the correspoding server function, hence the name.
 */
void
RecordNewMultiXact(MultiXactWriter *state, MultiXactOffset offset,
				   MultiXactId multi, int nmembers, MultiXactMember *members)
{
	int64		pageno;
	int64		prev_pageno;
	int			entryno;
	char	   *buf;
	MultiXactOffset *offptr;

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	/* Store the offset */
	buf = SlruWriteSwitchPage(state->offset, pageno);
	offptr = (MultiXactOffset *) buf;
	offptr[entryno] = offset;

	/* Store the members */
	prev_pageno = -1;
	for (int i = 0; i < nmembers; i++, offset++)
	{
		TransactionId *memberptr;
		uint32	   *flagsptr;
		uint32		flagsval;
		int			bshift;
		int			flagsoff;
		int			memberoff;

		Assert(members[i].status <= MultiXactStatusUpdate);

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);
		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);

		if (pageno != prev_pageno)
		{
			buf = SlruWriteSwitchPage(state->members, pageno);
			prev_pageno = pageno;
		}

		memberptr = (TransactionId *) (buf + memberoff);

		*memberptr = members[i].xid;

		flagsptr = (uint32 *) (buf + flagsoff);

		flagsval = *flagsptr;
		flagsval &= ~(((1 << MXACT_MEMBER_BITS_PER_XACT) - 1) << bshift);
		flagsval |= (members[i].status << bshift);
		*flagsptr = flagsval;
	}
}

void
FreeMultiXactWrite(MultiXactWriter *state)
{
	FreeSlruWrite(state->offset);
	FreeSlruWrite(state->members);

	pfree(state);
}
