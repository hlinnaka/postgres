/*-------------------------------------------------------------------------
 *
 * logicalworker.h
 *	  Exports for logical replication workers.
 *
 * Portions Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 * src/include/replication/logicalworker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICALWORKER_H
#define LOGICALWORKER_H

extern void ApplyWorkerMain(Datum main_arg);
extern void ParallelApplyWorkerMain(Datum main_arg);
extern void TableSyncWorkerMain(Datum main_arg);
extern void SequenceSyncWorkerMain(Datum main_arg);

extern bool IsLogicalWorker(void);
extern bool IsLogicalParallelApplyWorker(void);

extern void ProcessParallelApplyMessages(void);

extern void LogicalRepWorkersWakeupAtCommit(Oid subid);

extern void AtEOXact_LogicalRepWorkers(bool isCommit);

#endif							/* LOGICALWORKER_H */
