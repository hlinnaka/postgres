/*-------------------------------------------------------------------------
 *
 * miscadmin.h
 *	  This file contains general postgres administration and initialization
 *	  stuff that used to be spread out between the following files:
 *		globals.h						global variables
 *		pdir.h							directory path crud
 *		pinit.h							postgres initialization
 *		pmod.h							processing modes
 *	  Over time, this has also become the preferred place for widely known
 *	  resource-limitation stuff, such as work_mem and check_stack_depth().
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/miscadmin.h
 *
 * NOTES
 *	  some of the information in this file should be moved to other files.
 *
 *-------------------------------------------------------------------------
 */
#ifndef MISCADMIN_H
#define MISCADMIN_H

#include <signal.h>

#include "datatype/timestamp.h" /* for TimestampTz */
#include "pgtime.h"				/* for pg_time_t */

/*
 * XXX: many files include miscadmin.h for CHECK_FOR_INTERRUPTS(). Keep them
 * working without changes
 */
#ifndef FRONTEND
#include "ipc/interrupt.h"
#endif

#define InvalidPid				(-1)


/*****************************************************************************
 *	  globals.h --															 *
 *****************************************************************************/

/*
 * from utils/init/globals.c
 */
extern PGDLLIMPORT pid_t PostmasterPid;
extern PGDLLIMPORT bool IsPostmasterEnvironment;
extern PGDLLIMPORT bool IsUnderPostmaster;
extern PGDLLIMPORT bool IsBinaryUpgrade;

extern PGDLLIMPORT bool ExitOnAnyError;

extern PGDLLIMPORT char *DataDir;
extern PGDLLIMPORT int data_directory_mode;

extern PGDLLIMPORT int NBuffers;
extern PGDLLIMPORT int MaxBackends;
extern PGDLLIMPORT int MaxConnections;
extern PGDLLIMPORT int max_worker_processes;
extern PGDLLIMPORT int max_parallel_workers;

extern PGDLLIMPORT int commit_timestamp_buffers;
extern PGDLLIMPORT int multixact_member_buffers;
extern PGDLLIMPORT int multixact_offset_buffers;
extern PGDLLIMPORT int notify_buffers;
extern PGDLLIMPORT int serializable_buffers;
extern PGDLLIMPORT int subtransaction_buffers;
extern PGDLLIMPORT int transaction_buffers;

extern PGDLLIMPORT int MyProcPid;
extern PGDLLIMPORT pg_time_t MyStartTime;
extern PGDLLIMPORT TimestampTz MyStartTimestamp;
extern PGDLLIMPORT struct Port *MyProcPort;
extern PGDLLIMPORT uint8 MyCancelKey[];
extern PGDLLIMPORT int MyCancelKeyLength;
extern PGDLLIMPORT int MyPMChildSlot;

extern PGDLLIMPORT char OutputFileName[];
extern PGDLLIMPORT char my_exec_path[];
extern PGDLLIMPORT char pkglib_path[];

#ifdef EXEC_BACKEND
extern PGDLLIMPORT char postgres_exec_path[];
#endif

extern PGDLLIMPORT Oid MyDatabaseId;

extern PGDLLIMPORT Oid MyDatabaseTableSpace;

extern PGDLLIMPORT bool MyDatabaseHasLoginEventTriggers;

/*
 * Date/Time Configuration
 *
 * DateStyle defines the output formatting choice for date/time types:
 *	USE_POSTGRES_DATES specifies traditional Postgres format
 *	USE_ISO_DATES specifies ISO-compliant format
 *	USE_SQL_DATES specifies Oracle/Ingres-compliant format
 *	USE_GERMAN_DATES specifies German-style dd.mm/yyyy
 *
 * DateOrder defines the field order to be assumed when reading an
 * ambiguous date (anything not in YYYY-MM-DD format, with a four-digit
 * year field first, is taken to be ambiguous):
 *	DATEORDER_YMD specifies field order yy-mm-dd
 *	DATEORDER_DMY specifies field order dd-mm-yy ("European" convention)
 *	DATEORDER_MDY specifies field order mm-dd-yy ("US" convention)
 *
 * In the Postgres and SQL DateStyles, DateOrder also selects output field
 * order: day comes before month in DMY style, else month comes before day.
 *
 * The user-visible "DateStyle" run-time parameter subsumes both of these.
 */

/* valid DateStyle values */
#define USE_POSTGRES_DATES		0
#define USE_ISO_DATES			1
#define USE_SQL_DATES			2
#define USE_GERMAN_DATES		3
#define USE_XSD_DATES			4

/* valid DateOrder values */
#define DATEORDER_YMD			0
#define DATEORDER_DMY			1
#define DATEORDER_MDY			2

extern PGDLLIMPORT int DateStyle;
extern PGDLLIMPORT int DateOrder;

/*
 * IntervalStyles
 *	 INTSTYLE_POSTGRES			   Like Postgres < 8.4 when DateStyle = 'iso'
 *	 INTSTYLE_POSTGRES_VERBOSE	   Like Postgres < 8.4 when DateStyle != 'iso'
 *	 INTSTYLE_SQL_STANDARD		   SQL standard interval literals
 *	 INTSTYLE_ISO_8601			   ISO-8601-basic formatted intervals
 */
#define INTSTYLE_POSTGRES			0
#define INTSTYLE_POSTGRES_VERBOSE	1
#define INTSTYLE_SQL_STANDARD		2
#define INTSTYLE_ISO_8601			3

extern PGDLLIMPORT int IntervalStyle;

#define MAXTZLEN		10		/* max TZ name len, not counting tr. null */

extern PGDLLIMPORT bool enableFsync;
extern PGDLLIMPORT bool allowSystemTableMods;
extern PGDLLIMPORT int work_mem;
extern PGDLLIMPORT double hash_mem_multiplier;
extern PGDLLIMPORT int maintenance_work_mem;
extern PGDLLIMPORT int max_parallel_maintenance_workers;

/*
 * Upper and lower hard limits for the buffer access strategy ring size
 * specified by the VacuumBufferUsageLimit GUC and BUFFER_USAGE_LIMIT option
 * to VACUUM and ANALYZE.
 */
#define MIN_BAS_VAC_RING_SIZE_KB 128
#define MAX_BAS_VAC_RING_SIZE_KB (16 * 1024 * 1024)

extern PGDLLIMPORT int VacuumBufferUsageLimit;
extern PGDLLIMPORT int VacuumCostPageHit;
extern PGDLLIMPORT int VacuumCostPageMiss;
extern PGDLLIMPORT int VacuumCostPageDirty;
extern PGDLLIMPORT int VacuumCostLimit;
extern PGDLLIMPORT double VacuumCostDelay;

extern PGDLLIMPORT int VacuumCostBalance;
extern PGDLLIMPORT bool VacuumCostActive;


/* in utils/misc/stack_depth.c */

extern PGDLLIMPORT int max_stack_depth;

/* Required daylight between max_stack_depth and the kernel limit, in bytes */
#define STACK_DEPTH_SLOP (512 * 1024)

typedef char *pg_stack_base_t;

extern pg_stack_base_t set_stack_base(void);
extern void restore_stack_base(pg_stack_base_t base);
extern void check_stack_depth(void);
extern bool stack_is_too_deep(void);
extern ssize_t get_stack_depth_rlimit(void);

/* in tcop/utility.c */
extern void PreventCommandIfReadOnly(const char *cmdname);
extern void PreventCommandIfParallelMode(const char *cmdname);
extern void PreventCommandDuringRecovery(const char *cmdname);

/*****************************************************************************
 *	  pdir.h --																 *
 *			POSTGRES directory path definitions.                             *
 *****************************************************************************/

/* flags to be OR'd to form sec_context */
#define SECURITY_LOCAL_USERID_CHANGE	0x0001
#define SECURITY_RESTRICTED_OPERATION	0x0002
#define SECURITY_NOFORCE_RLS			0x0004

extern PGDLLIMPORT char *DatabasePath;

/* now in utils/init/miscinit.c */
extern void InitPostmasterChild(void);
extern void InitStandaloneProcess(const char *argv0);

/*
 * MyBackendType indicates what kind of a backend this is.
 *
 * If you add entries, please also update the child_process_kinds array in
 * launch_backend.c.
 */
typedef enum BackendType
{
	B_INVALID = 0,

	/* Backends and other backend-like processes */
	B_BACKEND,
	B_DEAD_END_BACKEND,
	B_AUTOVAC_LAUNCHER,
	B_AUTOVAC_WORKER,
	B_BG_WORKER,
	B_WAL_SENDER,
	B_SLOTSYNC_WORKER,

	B_STANDALONE_BACKEND,

	/*
	 * Auxiliary processes. These have PGPROC entries, but they are not
	 * attached to any particular database, and cannot run transactions or
	 * even take heavyweight locks. There can be only one of each of these
	 * running at a time, except for IO workers.
	 *
	 * If you modify these, make sure to update NUM_AUXILIARY_PROCS and the
	 * glossary in the docs.
	 */
	B_ARCHIVER,
	B_BG_WRITER,
	B_CHECKPOINTER,
	B_IO_WORKER,
	B_STARTUP,
	B_WAL_RECEIVER,
	B_WAL_SUMMARIZER,
	B_WAL_WRITER,

	/*
	 * Logger is not connected to shared memory and does not have a PGPROC
	 * entry.
	 */
	B_LOGGER,
} BackendType;

#define BACKEND_NUM_TYPES (B_LOGGER + 1)

extern PGDLLIMPORT BackendType MyBackendType;

#define AmRegularBackendProcess()	(MyBackendType == B_BACKEND)
#define AmAutoVacuumLauncherProcess() (MyBackendType == B_AUTOVAC_LAUNCHER)
#define AmAutoVacuumWorkerProcess()	(MyBackendType == B_AUTOVAC_WORKER)
#define AmBackgroundWorkerProcess() (MyBackendType == B_BG_WORKER)
#define AmWalSenderProcess()        (MyBackendType == B_WAL_SENDER)
#define AmLogicalSlotSyncWorkerProcess() (MyBackendType == B_SLOTSYNC_WORKER)
#define AmArchiverProcess()			(MyBackendType == B_ARCHIVER)
#define AmBackgroundWriterProcess() (MyBackendType == B_BG_WRITER)
#define AmCheckpointerProcess()		(MyBackendType == B_CHECKPOINTER)
#define AmStartupProcess()			(MyBackendType == B_STARTUP)
#define AmWalReceiverProcess()		(MyBackendType == B_WAL_RECEIVER)
#define AmWalSummarizerProcess()	(MyBackendType == B_WAL_SUMMARIZER)
#define AmWalWriterProcess()		(MyBackendType == B_WAL_WRITER)
#define AmIoWorkerProcess()			(MyBackendType == B_IO_WORKER)

#define AmSpecialWorkerProcess() \
	(AmAutoVacuumLauncherProcess() || \
	 AmLogicalSlotSyncWorkerProcess())

/*
 * Backend types that are spawned by the postmaster to serve a client or
 * replication connection. These backend types have in common that they are
 * externally initiated.
 */
#define IsExternalConnectionBackend(backend_type) \
	(backend_type == B_BACKEND || backend_type == B_WAL_SENDER)

extern const char *GetBackendTypeDesc(BackendType backendType);

extern void SetDatabasePath(const char *path);
extern void checkDataDir(void);
extern void SetDataDir(const char *dir);
extern void ChangeToDataDir(void);

extern char *GetUserNameFromId(Oid roleid, bool noerr);
extern Oid	GetUserId(void);
extern Oid	GetOuterUserId(void);
extern Oid	GetSessionUserId(void);
extern bool GetSessionUserIsSuperuser(void);
extern Oid	GetAuthenticatedUserId(void);
extern void SetAuthenticatedUserId(Oid userid);
extern void GetUserIdAndSecContext(Oid *userid, int *sec_context);
extern void SetUserIdAndSecContext(Oid userid, int sec_context);
extern bool InLocalUserIdChange(void);
extern bool InSecurityRestrictedOperation(void);
extern bool InNoForceRLSOperation(void);
extern void GetUserIdAndContext(Oid *userid, bool *sec_def_context);
extern void SetUserIdAndContext(Oid userid, bool sec_def_context);
extern void InitializeSessionUserId(const char *rolename, Oid roleid,
									bool bypass_login_check);
extern void InitializeSessionUserIdStandalone(void);
extern void SetSessionAuthorization(Oid userid, bool is_superuser);
extern Oid	GetCurrentRoleId(void);
extern void SetCurrentRoleId(Oid roleid, bool is_superuser);
extern void InitializeSystemUser(const char *authn_id,
								 const char *auth_method);
extern const char *GetSystemUser(void);

/* in utils/misc/superuser.c */
extern bool superuser(void);	/* current user is superuser */
extern bool superuser_arg(Oid roleid);	/* given user is superuser */


/*****************************************************************************
 *	  pmod.h --																 *
 *			POSTGRES processing mode definitions.                            *
 *****************************************************************************/

/*
 * Description:
 *		There are three processing modes in POSTGRES.  They are
 * BootstrapProcessing or "bootstrap," InitProcessing or
 * "initialization," and NormalProcessing or "normal."
 *
 * The first two processing modes are used during special times. When the
 * system state indicates bootstrap processing, transactions are all given
 * transaction id "one" and are consequently guaranteed to commit. This mode
 * is used during the initial generation of template databases.
 *
 * Initialization mode: used while starting a backend, until all normal
 * initialization is complete.  Some code behaves differently when executed
 * in this mode to enable system bootstrapping.
 *
 * If a POSTGRES backend process is in normal mode, then all code may be
 * executed normally.
 */

typedef enum ProcessingMode
{
	BootstrapProcessing,		/* bootstrap creation of template database */
	InitProcessing,				/* initializing system */
	NormalProcessing,			/* normal processing */
} ProcessingMode;

extern PGDLLIMPORT ProcessingMode Mode;

#define IsBootstrapProcessingMode() (Mode == BootstrapProcessing)
#define IsInitProcessingMode()		(Mode == InitProcessing)
#define IsNormalProcessingMode()	(Mode == NormalProcessing)

#define GetProcessingMode() Mode

#define SetProcessingMode(mode) \
	do { \
		Assert((mode) == BootstrapProcessing || \
				  (mode) == InitProcessing || \
				  (mode) == NormalProcessing); \
		Mode = (mode); \
	} while(0)


/*****************************************************************************
 *	  pinit.h --															 *
 *			POSTGRES initialization and cleanup definitions.                 *
 *****************************************************************************/

/* in utils/init/postinit.c */
/* flags for InitPostgres() */
#define INIT_PG_LOAD_SESSION_LIBS		0x0001
#define INIT_PG_OVERRIDE_ALLOW_CONNS	0x0002
#define INIT_PG_OVERRIDE_ROLE_LOGIN		0x0004
extern void pg_split_opts(char **argv, int *argcp, const char *optstr);
extern void InitializeMaxBackends(void);
extern void InitializeFastPathLocks(void);
extern void InitPostgres(const char *in_dbname, Oid dboid,
						 const char *username, Oid useroid,
						 bits32 flags,
						 char *out_dbname);
extern void BaseInit(void);
extern void StoreConnectionWarning(char *msg, char *detail);

/* in utils/init/miscinit.c */
extern PGDLLIMPORT bool IgnoreSystemIndexes;
extern PGDLLIMPORT bool process_shared_preload_libraries_in_progress;
extern PGDLLIMPORT bool process_shared_preload_libraries_done;
extern PGDLLIMPORT bool process_shmem_requests_in_progress;
extern PGDLLIMPORT char *session_preload_libraries_string;
extern PGDLLIMPORT char *shared_preload_libraries_string;
extern PGDLLIMPORT char *local_preload_libraries_string;

extern void CreateDataDirLockFile(bool amPostmaster);
extern void CreateSocketLockFile(const char *socketfile, bool amPostmaster,
								 const char *socketDir);
extern void TouchSocketLockFiles(void);
extern void AddToDataDirLockFile(int target_line, const char *str);
extern bool RecheckDataDirLockFile(void);
extern void ValidatePgVersion(const char *path);
extern void process_shared_preload_libraries(void);
extern void process_session_preload_libraries(void);
extern void process_shmem_requests(void);
extern void pg_bindtextdomain(const char *domain);
extern bool has_rolreplication(Oid roleid);

typedef void (*shmem_request_hook_type) (void);
extern PGDLLIMPORT shmem_request_hook_type shmem_request_hook;

extern Size EstimateClientConnectionInfoSpace(void);
extern void SerializeClientConnectionInfo(Size maxsize, char *start_address);
extern void RestoreClientConnectionInfo(char *conninfo);

/* in executor/nodeHash.c */
extern size_t get_hash_memory_limit(void);

#endif							/* MISCADMIN_H */
