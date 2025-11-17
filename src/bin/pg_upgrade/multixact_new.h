/*
 * multixact_new.h
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/multixact_new.h
 */
#ifndef MULTIXACT_NEW_H
#define MULTIXACT_NEW_H

#include "access/multixact.h"

#include "slru_io.h"

typedef struct MultiXactWriter
{
	SlruSegState *offset;
	SlruSegState *members;
} MultiXactWriter;

extern MultiXactWriter *AllocMultiXactWrite(const char *pgdata,
											MultiXactId firstMulti,
											MultiXactOffset firstOffset);
extern void RecordNewMultiXact(MultiXactWriter *state, MultiXactOffset offset,
							   MultiXactId multi, int nmembers,
							   MultiXactMember *members);
extern void FreeMultiXactWrite(MultiXactWriter *writer);

#endif							/* MULTIXACT_NEW_H */
