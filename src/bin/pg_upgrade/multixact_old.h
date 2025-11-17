/*
 * multixact_old.h
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/multixact_old.h
 */
#ifndef MULTIXACT_OLD_H
#define MULTIXACT_OLD_H

#include "access/multixact.h"
#include "slru_io.h"

/*
 * MultiXactOffset changed from uint32 to uint64 between versions 18 and 19.
 * OldMultiXactOffset is used to represent a 32-bit offset from the old
 * cluster.
 */
typedef uint32 OldMultiXactOffset;

typedef struct OldMultiXactReader
{
	MultiXactId nextMXact;
	OldMultiXactOffset nextOffset;

	SlruSegState *offset;
	SlruSegState *members;
} OldMultiXactReader;

extern OldMultiXactReader *AllocOldMultiXactRead(char *pgdata,
												 MultiXactId nextMulti,
												 OldMultiXactOffset nextOffset);
extern void GetOldMultiXactIdSingleMember(OldMultiXactReader *state,
										  MultiXactId multi,
										  TransactionId *result,
										  MultiXactStatus *status);
extern void FreeOldMultiXactReader(OldMultiXactReader *reader);

#endif							/* MULTIXACT_OLD_H */
