/*
 * multixact_old.h
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/multixact_old.h
 */

#include "access/multixact.h"
#include "slru_io.h"

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
