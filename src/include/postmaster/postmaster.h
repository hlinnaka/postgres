/*-------------------------------------------------------------------------
 *
 * postmaster.h
 *	  Exports from postmaster/postmaster.c.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/postmaster.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _POSTMASTER_H
#define _POSTMASTER_H

#include "lib/ilist.h"
#include "miscadmin.h"

/*
 * A struct representing an active postmaster child process.  This is used
 * mainly to keep track of how many children we have and send them appropriate
 * signals when necessary.  All postmaster child processes are assigned a
 * PMChild entry.  That includes "normal" client sessions, but also autovacuum
 * workers, walsenders, background workers, and aux processes.  (Note that at
 * the time of launch, walsenders are labeled B_BACKEND; we relabel them to
 * B_WAL_SENDER upon noticing they've changed their PMChildFlags entry.  Hence
 * that check must be done before any operation that needs to distinguish
 * walsenders from normal backends.)
 *
 * "dead-end" children are also allocated a PMChild entry: these are children
 * launched just for the purpose of sending a friendly rejection message to a
 * would-be client.  We must track them because they are attached to shared
 * memory, but we know they will never become live backends.
 *
 * child_slot is an identifier that is unique across all running child
 * processes.  It is used as an index into the PMChildFlags array.  dead-end
 * children are not assigned a child_slot and have child_slot == 0 (valid
 * child_slot ids start from 1).
 */

typedef union
{
	pid_t		pid;
	pthread_t	threadid;
} pid_or_threadid;

static inline bool
pid_eq(pid_or_threadid a, pid_or_threadid b)
{
	return IsMultiThreaded ? (a.threadid == b.threadid) : (a.pid == b.pid);
}

typedef struct PMChild
{
	pid_or_threadid	pid;			/* process id of backend */
	int			child_slot;		/* PMChildSlot for this backend, if any */
	BackendType bkend_type;		/* child process flavor, see above */
	struct RegisteredBgWorker *rw;	/* bgworker info, if this is a bgworker */
	bool		bgworker_notify;	/* gets bgworker start/stop notifications */
	dlist_node	elem;			/* list link in ActiveChildList */
} PMChild;

#ifdef EXEC_BACKEND
extern pg_global int	num_pmchild_slots;
#endif

extern void thread_pre_exit(pthread_t threadid, int code);

/* GUC options */
extern PGDLLIMPORT sighup_guc bool EnableSSL;
extern PGDLLIMPORT postmaster_guc int SuperuserReservedConnections;
extern PGDLLIMPORT postmaster_guc int ReservedConnections;
extern PGDLLIMPORT postmaster_guc int PostPortNumber;
extern PGDLLIMPORT postmaster_guc int Unix_socket_permissions;
extern PGDLLIMPORT postmaster_guc char *Unix_socket_group;
extern PGDLLIMPORT postmaster_guc char *Unix_socket_directories;
extern PGDLLIMPORT postmaster_guc char *ListenAddresses;
extern PGDLLIMPORT session_local bool ClientAuthInProgress;
extern PGDLLIMPORT sighup_guc int PreAuthDelay;
extern PGDLLIMPORT sighup_guc int AuthenticationTimeout;
extern PGDLLIMPORT session_guc bool Log_connections;
extern PGDLLIMPORT sighup_guc bool log_hostname;
extern PGDLLIMPORT postmaster_guc bool enable_bonjour;
extern PGDLLIMPORT postmaster_guc char *bonjour_name;
extern PGDLLIMPORT sighup_guc bool restart_after_crash;
extern PGDLLIMPORT sighup_guc bool remove_temp_files_after_crash;
extern PGDLLIMPORT sighup_guc bool send_abort_for_crash;
extern PGDLLIMPORT sighup_guc bool send_abort_for_kill;

#ifdef WIN32
extern PGDLLIMPORT HANDLE PostmasterHandle;
#else
extern pg_global PGDLLIMPORT int postmaster_alive_fds[2];

/*
 * Constants that represent which of postmaster_alive_fds is held by
 * postmaster, and which is used in children to check for postmaster death.
 */
#define POSTMASTER_FD_WATCH		0	/* used in children to check for
									 * postmaster death */
#define POSTMASTER_FD_OWN		1	/* kept open by postmaster only */
#endif

extern PGDLLIMPORT const char *progname;

extern pg_global PGDLLIMPORT bool redirection_done;
extern pg_global PGDLLIMPORT bool LoadedSSL;

extern void PostmasterMain(int argc, char *argv[]) pg_attribute_noreturn();
extern void ClosePostmasterPorts(bool am_syslogger);
extern void InitProcessGlobals(void);

extern int	MaxLivePostmasterChildren(void);

extern bool PostmasterMarkPIDForWorkerNotify(int);
extern void signal_child(PMChild *pmchild, int signal);

#ifdef WIN32
extern void pgwin32_register_deadchild_callback(HANDLE procHandle, DWORD procId);
#endif

extern void handle_pm_pmsignal_signal(SIGNAL_ARGS);

/* defined in globals.c */
extern PGDLLIMPORT session_local struct ClientSocket *MyClientSocket;

/* prototypes for functions in launch_backend.c */
extern bool postmaster_child_launch(BackendType child_type,
									 int child_slot,
									 char *startup_data,
									 size_t startup_data_len,
									 struct ClientSocket *client_sock,
									 pid_or_threadid *id);
const char *PostmasterChildName(BackendType child_type);
#ifdef EXEC_BACKEND
extern void SubPostmasterMain(int argc, char *argv[]) pg_attribute_noreturn();
#endif

/* defined in pmchild.c */
extern pg_global dlist_head ActiveChildList;

extern void InitPostmasterChildSlots(void);
extern PMChild *AssignPostmasterChildSlot(BackendType btype);
extern PMChild *AllocDeadEndChild(void);
extern bool ReleasePostmasterChildSlot(PMChild *pmchild);
extern PMChild *FindPostmasterChildByPid(pid_or_threadid id);

/*
 * Note: MAX_BACKENDS is limited to 2^18-1 because that's the width reserved
 * for buffer references in buf_internals.h.  This limitation could be lifted
 * by using a 64bit state; but it's unlikely to be worthwhile as 2^18-1
 * backends exceed currently realistic configurations. Even if that limitation
 * were removed, we still could not a) exceed 2^23-1 because inval.c stores
 * the ProcNumber as a 3-byte signed integer, b) INT_MAX/4 because some places
 * compute 4*MaxBackends without any overflow check.  This is rechecked in the
 * relevant GUC check hooks and in RegisterBackgroundWorker().
 */
#define MAX_BACKENDS	0x3FFFF

#endif							/* _POSTMASTER_H */
