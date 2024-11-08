/*-------------------------------------------------------------------------
 *
 * globals.c
 *	  global variable declarations
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/globals.c
 *
 * NOTES
 *	  Globals used all over the place should be declared here and not
 *	  in other modules.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/file_perm.h"
#include "libpq/libpq-be.h"
#include "libpq/pqcomm.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/procnumber.h"
#include "storage/procsignal.h"


session_local ProtocolVersion FrontendProtocol;

session_local volatile uint32 InterruptHoldoffCount = 0;
session_local volatile uint32 QueryCancelHoldoffCount = 0;
session_local volatile uint32 CritSectionCount = 0;

session_local volatile uint32 CheckForInterruptsMask = 0;

session_local int			MyProcPid;
session_local pg_time_t	MyStartTime;
session_local TimestampTz MyStartTimestamp;
session_local struct ClientSocket *MyClientSocket;
session_local struct Port *MyProcPort;
session_local uint8		MyCancelKey[MAX_CANCEL_KEY_LENGTH];
session_local int			MyCancelKeyLength = 0;
session_local int			MyPMChildSlot;

/*
 * DataDir is the absolute path to the top level of the PGDATA directory tree.
 * Except during early startup, this is also the server's working directory;
 * most code therefore can simply use relative paths and not reference DataDir
 * explicitly.
 */
pg_global char	   *DataDir = NULL;

/*
 * Mode of the data directory.  The default is 0700 but it may be changed in
 * checkDataDir() to 0750 if the data directory actually has that mode.
 */
session_local int			data_directory_mode = PG_DIR_MODE_OWNER;

dynamic_singleton char		OutputFileName[MAXPGPATH];	/* debugging output file */

dynamic_singleton char		my_exec_path[MAXPGPATH];	/* full path to my executable */
dynamic_singleton char		pkglib_path[MAXPGPATH]; /* full path to lib directory */

#ifdef EXEC_BACKEND
char		postgres_exec_path[MAXPGPATH];	/* full path to backend */

/* note: currently this is not valid in backend processes */
#endif

session_local ProcNumber	MyProcNumber = INVALID_PROC_NUMBER;

session_local ProcNumber	ParallelLeaderProcNumber = INVALID_PROC_NUMBER;

session_local Oid			MyDatabaseId = InvalidOid;

session_local Oid			MyDatabaseTableSpace = InvalidOid;

session_local bool		MyDatabaseHasLoginEventTriggers = false;

/*
 * DatabasePath is the path (relative to DataDir) of my database's
 * primary directory, ie, its directory in the default tablespace.
 */
session_local char	   *DatabasePath = NULL;

pg_global pid_t	PostmasterPid = 0;

/*
 * IsPostmasterEnvironment is true in a postmaster process and any postmaster
 * child process; it is false in a standalone process (bootstrap or
 * standalone backend).  IsUnderPostmaster is true in postmaster child
 * processes.  Note that "child process" includes all children, not only
 * regular backends.  These should be set correctly as early as possible
 * in the execution of a process, so that error handling will do the right
 * things if an error should occur during process initialization.
 *
 * These are initialized for the bootstrap/standalone case.
 */
pg_global bool		IsPostmasterEnvironment = false;
session_local bool		IsUnderPostmaster = false;
pg_global bool		IsBinaryUpgrade = false;

session_guc bool		ExitOnAnyError = false;

postmaster_guc bool		IsMultiThreaded = true; /* GUC */

session_local int			DateStyle = USE_ISO_DATES;
session_local int			DateOrder = DATEORDER_MDY;
session_guc int			IntervalStyle = INTSTYLE_POSTGRES;

sighup_guc bool		enableFsync = true;
session_guc bool		allowSystemTableMods = false;
session_guc int			work_mem = 4096;
session_guc double		hash_mem_multiplier = 2.0;
session_guc int			maintenance_work_mem = 65536;
session_guc int			max_parallel_maintenance_workers = 2;

/*
 * Primary determinants of sizes of shared-memory structures.
 *
 * MaxBackends is computed by PostmasterMain after modules have had a chance to
 * register background workers.
 */
postmaster_guc int		NBuffers = 16384;
postmaster_guc int		MaxConnections = 100;
postmaster_guc int		max_worker_processes = 8;
session_guc int			max_parallel_workers = 8;
dynamic_singleton int	MaxBackends = 0;

/* GUC parameters for vacuum */
session_guc int			VacuumBufferUsageLimit = 2048;

session_guc int			VacuumCostPageHit = 1;
session_guc int			VacuumCostPageMiss = 2;
session_guc int			VacuumCostPageDirty = 20;
session_guc int			VacuumCostLimit = 200;
session_guc double		VacuumCostDelay = 0;

session_local int			VacuumCostBalance = 0;	/* working state for vacuum */
session_local bool		VacuumCostActive = false;

/* configurable SLRU buffer sizes */
postmaster_guc int			commit_timestamp_buffers = 0;
postmaster_guc int			multixact_member_buffers = 32;
postmaster_guc int			multixact_offset_buffers = 16;
postmaster_guc int			notify_buffers = 16;
postmaster_guc int			serializable_buffers = 32;
postmaster_guc int			subtransaction_buffers = 0;
postmaster_guc int			transaction_buffers = 0;
