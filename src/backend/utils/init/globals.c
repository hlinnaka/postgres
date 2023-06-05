/*-------------------------------------------------------------------------
 *
 * globals.c
 *	  global variable declarations
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
#include "storage/backendid.h"


ProtocolVersion FrontendProtocol;

session_local volatile sig_atomic_t InterruptPending = false;
session_local volatile sig_atomic_t QueryCancelPending = false;
session_local volatile sig_atomic_t ProcDiePending = false;
session_local volatile sig_atomic_t CheckClientConnectionPending = false;
session_local volatile sig_atomic_t ClientConnectionLost = false;
session_local volatile sig_atomic_t IdleInTransactionSessionTimeoutPending = false;
session_local volatile sig_atomic_t IdleSessionTimeoutPending = false;
session_local volatile sig_atomic_t ProcSignalBarrierPending = false;
session_local volatile sig_atomic_t LogMemoryContextPending = false;
session_local volatile sig_atomic_t IdleStatsUpdateTimeoutPending = false;
session_local volatile uint32 InterruptHoldoffCount = 0;
session_local volatile uint32 QueryCancelHoldoffCount = 0;
session_local volatile uint32 CritSectionCount = 0;

session_local int			MyProcPid;
session_local pg_time_t	MyStartTime;
session_local TimestampTz MyStartTimestamp;
session_local struct Port *MyProcPort;
session_local int32		MyCancelKey;
session_local int			MyPMChildSlot;

/*
 * MyLatch points to the latch that should be used for signal handling by the
 * current process. It will either point to a process local latch if the
 * current process does not have a PGPROC entry in that moment, or to
 * PGPROC->procLatch if it has. Thus it can always be used in signal handlers,
 * without checking for its existence.
 */
session_local struct Latch *MyLatch;

/*
 * DataDir is the absolute path to the top level of the PGDATA directory tree.
 * Except during early startup, this is also the server's working directory;
 * most code therefore can simply use relative paths and not reference DataDir
 * explicitly.
 */
session_local char	   *DataDir = NULL;

/*
 * Mode of the data directory.  The default is 0700 but it may be changed in
 * checkDataDir() to 0750 if the data directory actually has that mode.
 */
session_local int			data_directory_mode = PG_DIR_MODE_OWNER;

char		OutputFileName[MAXPGPATH];	/* debugging output file */

char		my_exec_path[MAXPGPATH];	/* full path to my executable */
char		pkglib_path[MAXPGPATH]; /* full path to lib directory */

#ifdef EXEC_BACKEND
char		postgres_exec_path[MAXPGPATH];	/* full path to backend */

/* note: currently this is not valid in backend processes */
#endif

session_local BackendId	MyBackendId = InvalidBackendId;

session_local BackendId	ParallelLeaderBackendId = InvalidBackendId;

session_local Oid			MyDatabaseId = InvalidOid;

session_local Oid			MyDatabaseTableSpace = InvalidOid;

bool		MyDatabaseHasLoginEventTriggers = false;

/*
 * DatabasePath is the path (relative to DataDir) of my database's
 * primary directory, ie, its directory in the default tablespace.
 */
session_local char	   *DatabasePath = NULL;

pid_t		PostmasterPid = 0;

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
bool		IsPostmasterEnvironment = false;
bool		IsUnderPostmaster = false;
bool		IsBinaryUpgrade = false;
session_local bool		IsBackgroundWorker = false;

bool		IsMultiThreaded = false; /* GUC */

bool		ExitOnAnyError = false;

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
postmaster_guc int			NBuffers = 16384;
postmaster_guc int			MaxConnections = 100;
postmaster_guc int			max_worker_processes = 8;
session_guc int			max_parallel_workers = 8;
int			MaxBackends = 0;

/* GUC parameters for vacuum */
session_guc int			VacuumBufferUsageLimit = 256;

session_guc int			VacuumCostPageHit = 1;
session_guc int			VacuumCostPageMiss = 2;
session_guc int			VacuumCostPageDirty = 20;
session_guc int			VacuumCostLimit = 200;
session_guc double		VacuumCostDelay = 0;

session_local int64		VacuumPageHit = 0;
session_local int64		VacuumPageMiss = 0;
session_local int64		VacuumPageDirty = 0;

session_local int			VacuumCostBalance = 0;	/* working state for vacuum */
session_local bool		VacuumCostActive = false;
