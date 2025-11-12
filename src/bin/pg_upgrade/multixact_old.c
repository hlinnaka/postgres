/*
 * multixact_old.c
 *
 * Functions to read pre-v19 multixacts
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/multixact_old.c
 */

#include "postgres_fe.h"

#include "multixact_old.h"
#include "pg_upgrade.h"

/*
 * NOTE: below are a bunch of definitions that are copy-pasted from
 * multixact.c from version 18.  The only difference is that we use the
 * OldMultiXactOffset type equal to uint32 instead of MultiXactOffset which
 * became uint64.
 */

/* We need four bytes per offset and 8 bytes per base for each page. */
#define MULTIXACT_OFFSETS_PER_PAGE (BLCKSZ / sizeof(OldMultiXactOffset))

static inline int64
MultiXactIdToOffsetPage(MultiXactId multi)
{
	return multi / MULTIXACT_OFFSETS_PER_PAGE;
}

static inline int
MultiXactIdToOffsetEntry(MultiXactId multi)
{
	return multi % MULTIXACT_OFFSETS_PER_PAGE;
}

/*
 * The situation for members is a bit more complex: we store one byte of
 * additional flag bits for each TransactionId.  To do this without getting
 * into alignment issues, we store four bytes of flags, and then the
 * corresponding 4 Xids.  Each such 5-word (20-byte) set we call a "group", and
 * are stored as a whole in pages.  Thus, with 8kB BLCKSZ, we keep 409 groups
 * per page.  This wastes 12 bytes per page, but that's OK -- simplicity (and
 * performance) trumps space efficiency here.
 *
 * Note that the "offset" macros work with byte offset, not array indexes, so
 * arithmetic must be done using "char *" pointers.
 */
/* We need eight bits per xact, so one xact fits in a byte */
#define MXACT_MEMBER_BITS_PER_XACT			8
#define MXACT_MEMBER_FLAGS_PER_BYTE			1
#define MXACT_MEMBER_XACT_BITMASK	((1 << MXACT_MEMBER_BITS_PER_XACT) - 1)

/* how many full bytes of flags are there in a group? */
#define MULTIXACT_FLAGBYTES_PER_GROUP		4
#define MULTIXACT_MEMBERS_PER_MEMBERGROUP	\
	(MULTIXACT_FLAGBYTES_PER_GROUP * MXACT_MEMBER_FLAGS_PER_BYTE)
/* size in bytes of a complete group */
#define MULTIXACT_MEMBERGROUP_SIZE \
	(sizeof(TransactionId) * MULTIXACT_MEMBERS_PER_MEMBERGROUP + MULTIXACT_FLAGBYTES_PER_GROUP)
#define MULTIXACT_MEMBERGROUPS_PER_PAGE (BLCKSZ / MULTIXACT_MEMBERGROUP_SIZE)
#define MULTIXACT_MEMBERS_PER_PAGE	\
	(MULTIXACT_MEMBERGROUPS_PER_PAGE * MULTIXACT_MEMBERS_PER_MEMBERGROUP)

/* page in which a member is to be found */
static inline int64
MXOffsetToMemberPage(OldMultiXactOffset offset)
{
	return offset / MULTIXACT_MEMBERS_PER_PAGE;
}

/* Location (byte offset within page) of flag word for a given member */
static inline int
MXOffsetToFlagsOffset(MultiXactOffset offset)
{
	OldMultiXactOffset group = offset / MULTIXACT_MEMBERS_PER_MEMBERGROUP;
	int			grouponpg = group % MULTIXACT_MEMBERGROUPS_PER_PAGE;
	int			byteoff = grouponpg * MULTIXACT_MEMBERGROUP_SIZE;

	return byteoff;
}

/* Location (byte offset within page) of TransactionId of given member */
static inline int
MXOffsetToMemberOffset(OldMultiXactOffset offset)
{
	int			member_in_group = offset % MULTIXACT_MEMBERS_PER_MEMBERGROUP;

	return MXOffsetToFlagsOffset(offset) +
		MULTIXACT_FLAGBYTES_PER_GROUP +
		member_in_group * sizeof(TransactionId);
}

static inline int
MXOffsetToFlagsBitShift(OldMultiXactOffset offset)
{
	int			member_in_group = offset % MULTIXACT_MEMBERS_PER_MEMBERGROUP;
	int			bshift = member_in_group * MXACT_MEMBER_BITS_PER_XACT;

	return bshift;
}

/*
 * Construct reader of old multixacts.
 *
 * Returns the malloced memory used by the all other calls in this module.
 */
OldMultiXactReader *
AllocOldMultiXactRead(char *pgdata, MultiXactId nextMulti,
					  OldMultiXactOffset nextOffset)
{
	OldMultiXactReader *state = state = pg_malloc(sizeof(*state));
	char		dir[MAXPGPATH] = {0};

	state->nextMXact = nextMulti;
	state->nextOffset = nextOffset;

	pg_sprintf(dir, "%s/pg_multixact/offsets", pgdata);
	state->offset = AllocSlruRead(dir, false);

	pg_sprintf(dir, "%s/pg_multixact/members", pgdata);
	state->members = AllocSlruRead(dir, false);

	return state;
}

/*
 * This is a simplified version of the GetMultiXactIdMembers() server function.
 *
 * - Only return the updating member, if any. Upgrade only cares about the
 *   updaters. If there is no updating member, return the first locking-only
 *   member. We don't have any way to represent "no members", but we also don't
 *   need to preserve all the locking members.
 *
 * - We don't need to worry about locking and some corner cases because there's
 *   no concurrent activity.
 */
void
GetOldMultiXactIdSingleMember(OldMultiXactReader *state, MultiXactId multi,
							  TransactionId *result, MultiXactStatus *status)
{
	MultiXactId nextMXact,
				nextOffset,
				tmpMXact;
	int64		pageno,
				prev_pageno;
	int			entryno,
				length;
	char	   *buf;
	OldMultiXactOffset *offptr,
				offset;
	TransactionId result_xid = InvalidTransactionId;
	bool		result_isupdate = false;

	nextMXact = state->nextMXact;
	nextOffset = state->nextOffset;

	/*
	 * See GetMultiXactIdMembers in multixact.c
	 *
	 * Find out the offset at which we need to start reading MultiXactMembers
	 * and the number of members in the multixact.  We determine the latter as
	 * the difference between this multixact's starting offset and the next
	 * one's.  However, there are some corner cases to worry about:
	 *
	 * 1. This multixact may be the latest one created, in which case there is
	 * no next one to look at.  In this case the nextOffset value we just
	 * saved is the correct endpoint.
	 *
	 * 2. The next multixact may still be in process of being filled in...
	 * This cannot happen during upgrade.
	 *
	 * 3. Because GetNewMultiXactId increments offset zero to offset one to
	 * handle case #2, there is an ambiguity near the point of offset
	 * wraparound.  If we see next multixact's offset is one, is that our
	 * multixact's actual endpoint, or did it end at zero with a subsequent
	 * increment?  We handle this using the knowledge that if the zero'th
	 * member slot wasn't filled, it'll contain zero, and zero isn't a valid
	 * transaction ID so it can't be a multixact member.  Therefore, if we
	 * read a zero from the members array, just ignore it.
	 */

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	buf = SlruReadSwitchPage(state->offset, pageno);
	offptr = (OldMultiXactOffset *) buf;
	offptr += entryno;
	offset = *offptr;

	Assert(offset != 0);

	/*
	 * Use the same increment rule as GetNewMultiXactId(), that is, don't
	 * handle wraparound explicitly until needed.
	 */
	tmpMXact = multi + 1;

	if (nextMXact == tmpMXact)
	{
		/* Corner case 1: there is no next multixact */
		length = nextOffset - offset;
	}
	else
	{
		OldMultiXactOffset nextMXOffset;

		/* handle wraparound if needed */
		if (tmpMXact < FirstMultiXactId)
			tmpMXact = FirstMultiXactId;

		prev_pageno = pageno;

		pageno = MultiXactIdToOffsetPage(tmpMXact);
		entryno = MultiXactIdToOffsetEntry(tmpMXact);

		if (pageno != prev_pageno)
			buf = SlruReadSwitchPage(state->offset, pageno);

		offptr = (OldMultiXactOffset *) buf;
		offptr += entryno;
		nextMXOffset = *offptr;

		/*
		 * Corner case 2: next multixact is still being filled in, this must
		 * not happen during upgrade.
		 */
		Assert(nextMXOffset != 0);

		length = nextMXOffset - offset;
	}

	prev_pageno = -1;
	for (int i = 0; i < length; i++, offset++)
	{
		TransactionId *xactptr;
		uint32	   *flagsptr;
		int			flagsoff;
		int			bshift;
		int			memberoff;
		MultiXactStatus st;

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);

		if (pageno != prev_pageno)
		{
			buf = SlruReadSwitchPage(state->members, pageno);
			prev_pageno = pageno;
		}

		xactptr = (TransactionId *) (buf + memberoff);
		if (!TransactionIdIsValid(*xactptr))
		{
			/* Corner case 3: we must be looking at unused slot zero */
			Assert(offset == 0);
			continue;
		}

		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);
		flagsptr = (uint32 *) (buf + flagsoff);

		st = (*flagsptr >> bshift) & MXACT_MEMBER_XACT_BITMASK;

		/* Verify that there is a single update Xid among the given members. */
		if (ISUPDATE_from_mxstatus(st))
		{
			if (result_isupdate)
				pg_fatal("multixact %u has more than one updating member",
						 multi);
			result_xid = *xactptr;
			result_isupdate = true;
		}
		else if (!TransactionIdIsValid(result_xid))
			result_xid = *xactptr;
	}

	/* A multixid with zero members should not happen */
	Assert(TransactionIdIsValid(result_xid));

	*result = result_xid;
	*status = result_isupdate ? MultiXactStatusUpdate :
		MultiXactStatusForKeyShare;
}

/*
 * Frees the malloced reader.
 */
void
FreeOldMultiXactReader(OldMultiXactReader *state)
{
	FreeSlruRead(state->offset);
	FreeSlruRead(state->members);

	pfree(state);
}
