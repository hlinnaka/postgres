/*-------------------------------------------------------------------------
 *
 * waiteventset.c
 *	  ppoll()/pselect() like abstraction
 *
 * WaitEvents are an abstraction for waiting for one or more events at a time.
 * The waiting can be done in a race free fashion, similar ppoll() or
 * pselect() (as opposed to plain poll()/select()).
 *
 * You can wait for:
 * - an interrupt from another process or from signal handler in the same
 *   process (WL_INTERRUPT)
 * - data to become readable or writeable on a socket (WL_SOCKET_*)
 * - postmaster death (WL_POSTMASTER_DEATH or WL_EXIT_ON_PM_DEATH)
 * - timeout (WL_TIMEOUT)
 *
 * Implementation
 * --------------
 *
 * The poll() implementation uses the so-called self-pipe trick to overcome
 * the race condition involved with poll() and setting a global flag in the
 * signal handler. When an interrupt is received and the current process is
 * waiting for it, the signal handler wakes up the poll() in WaitInterrupt by
 * writing a byte to a pipe.  A signal by itself doesn't interrupt poll() on
 * all platforms, and even on platforms where it does, a signal that arrives
 * just before the poll() call does not prevent poll() from entering sleep. An
 * incoming byte on a pipe however reliably interrupts the sleep, and causes
 * poll() to return immediately even if the signal arrives before poll()
 * begins.
 *
 * The epoll() implementation overcomes the race with a different technique: it
 * keeps SIGURG blocked and consumes from a signalfd() descriptor instead.  We
 * don't need to register a signal handler or create our own self-pipe.  We
 * assume that any system that has Linux epoll() also has Linux signalfd().
 *
 * The kqueue() implementation waits for SIGURG with EVFILT_SIGNAL.
 *
 * The Windows implementation uses Windows events that are inherited by all
 * postmaster child processes. There's no need for the self-pipe trick there.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/waiteventset.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#ifdef HAVE_SYS_EPOLL_H
#include <sys/epoll.h>
#endif
#ifdef HAVE_SYS_EVENT_H
#include <sys/event.h>
#endif
#ifdef HAVE_SYS_SIGNALFD_H
#include <sys/signalfd.h>
#endif
#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "portability/instr_time.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/interrupt.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/waiteventset.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

/*
 * Select the fd readiness primitive to use. Normally the "most modern"
 * primitive supported by the OS will be used, but for testing it can be
 * useful to manually specify the used primitive.  If desired, just add a
 * define somewhere before this block.
 */
#if defined(WAIT_USE_EPOLL) || defined(WAIT_USE_POLL) || \
	defined(WAIT_USE_KQUEUE) || defined(WAIT_USE_WIN32)
/* don't overwrite manual choice */
#elif defined(HAVE_SYS_EPOLL_H)
#define WAIT_USE_EPOLL
#elif defined(HAVE_KQUEUE)
#define WAIT_USE_KQUEUE
#elif defined(HAVE_POLL)
#define WAIT_USE_POLL
#elif WIN32
#define WAIT_USE_WIN32
#else
#error "no wait set implementation available"
#endif

/*
 * By default, we use a self-pipe with poll() and a signalfd with epoll(), if
 * available.  For testing the choice can also be manually specified.
 */
#if defined(WAIT_USE_POLL) || defined(WAIT_USE_EPOLL)
#if defined(WAIT_USE_SELF_PIPE) || defined(WAIT_USE_SIGNALFD)
/* don't overwrite manual choice */
#elif defined(WAIT_USE_EPOLL) && defined(HAVE_SYS_SIGNALFD_H)
#define WAIT_USE_SIGNALFD
#else
#define WAIT_USE_SELF_PIPE
#endif
#endif

/* typedef in waiteventset.h */
struct WaitEventSet
{
	ResourceOwner owner;

	int			nevents;		/* number of registered events */
	int			nevents_space;	/* maximum number of events in this set */

	/*
	 * Array, of nevents_space length, storing the definition of events this
	 * set is waiting for.
	 */
	WaitEvent  *events;

	/*
	 * If WL_INTERRUPT is specified in any wait event, interruptMask is the
	 * interrupts to wait for, and interrupt_pos the offset in the ->events
	 * array. This is useful because we check the state of pending interrupts
	 * before performing doing syscalls related to waiting.
	 */
	uint32		interrupt_mask;
	int			interrupt_pos;

	/*
	 * WL_EXIT_ON_PM_DEATH is converted to WL_POSTMASTER_DEATH, but this flag
	 * is set so that we'll exit immediately if postmaster death is detected,
	 * instead of returning.
	 */
	bool		exit_on_postmaster_death;

#if defined(WAIT_USE_EPOLL)
	int			epoll_fd;
	/* epoll_wait returns events in a user provided arrays, allocate once */
	struct epoll_event *epoll_ret_events;
#elif defined(WAIT_USE_KQUEUE)
	int			kqueue_fd;
	/* kevent returns events in a user provided arrays, allocate once */
	struct kevent *kqueue_ret_events;
	bool		report_postmaster_not_running;
#elif defined(WAIT_USE_POLL)
	/* poll expects events to be waited on every poll() call, prepare once */
	struct pollfd *pollfds;
#elif defined(WAIT_USE_WIN32)

	/*
	 * Array of windows events. The first element always contains
	 * pgwin32_signal_event, so the remaining elements are offset by one (i.e.
	 * event->pos + 1).
	 */
	HANDLE	   *handles;
#endif
};

/* A common WaitEventSet used to implement WaitInterrupt() */
static session_local WaitEventSet *InterruptWaitSet;

/* The position of the interrupt in InterruptWaitSet. */
#define InterruptWaitSetInterruptPos 0

#ifndef WIN32
/* Are we currently in WaitInterrupt? The signal handler would like to know. */
static session_local volatile sig_atomic_t waiting = false;
#endif

#ifdef WAIT_USE_SIGNALFD
/* On Linux, we'll receive SIGURG via a signalfd file descriptor. */
static session_local int	signal_fd = -1; /* This will require some additional changes. See comment in InitializeWaitEventSupport */
#endif

#ifdef WAIT_USE_SELF_PIPE
/* Read and write ends of the self-pipe */
static session_local int	selfpipe_readfd = -1;
static session_local int	selfpipe_writefd = -1;

/* Process owning the self-pipe --- needed for checking purposes */
static session_local int	selfpipe_owner_pid = 0;
#endif

#ifdef WAIT_USE_WIN32
static session_local HANDLE LocalInterruptWakeupEvent;
#endif

/* Private function prototypes */

#ifdef WAIT_USE_SELF_PIPE
static void interrupt_sigurg_handler(SIGNAL_ARGS);
static void sendSelfPipeByte(void);
#endif

static void sendPipeWakeupByte(int writefd);

static void drain(void);

#if defined(WAIT_USE_EPOLL)
static void WaitEventAdjustEpoll(WaitEventSet *set, WaitEvent *event, int action);
#elif defined(WAIT_USE_KQUEUE)
static void WaitEventAdjustKqueue(WaitEventSet *set, WaitEvent *event, int old_events);
#elif defined(WAIT_USE_POLL)
static void WaitEventAdjustPoll(WaitEventSet *set, WaitEvent *event);
#elif defined(WAIT_USE_WIN32)
static void WaitEventAdjustWin32(WaitEventSet *set, WaitEvent *event);
#endif

/*
 * Multithreaded implementation
 *
 * There's a pipe, similar to the self-pipe, for each pmchild slot. And element 0 for
 * postmaster
 */
#define POSTMASTER_PMCHILD_SLOT		INT_MAX

static pg_global int *thread_wakeup_readfds;
static pg_global int *thread_wakeup_writefds;



static void InitializeWaitEventSupportProcess(void);
static inline int WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
										WaitEvent *occurred_events, int nevents);

/* ResourceOwner support to hold WaitEventSets */
static void ResOwnerReleaseWaitEventSet(Datum res);

static const ResourceOwnerDesc wait_event_set_resowner_desc =
{
	.name = "WaitEventSet",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_WAITEVENTSETS,
	.ReleaseResource = ResOwnerReleaseWaitEventSet,
	.DebugPrint = NULL
};

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void
ResourceOwnerRememberWaitEventSet(ResourceOwner owner, WaitEventSet *set)
{
	ResourceOwnerRemember(owner, PointerGetDatum(set), &wait_event_set_resowner_desc);
}
static inline void
ResourceOwnerForgetWaitEventSet(ResourceOwner owner, WaitEventSet *set)
{
	ResourceOwnerForget(owner, PointerGetDatum(set), &wait_event_set_resowner_desc);
}


/*
 * Initialize the process-local wait event infrastructure.
 *
 * This must be called once during startup of any process that can wait on
 * interrupts.
 */
void
InitializeWaitEventSupport(void)
{
	int			n;

	if (!IsMultiThreaded)
	{
		InitializeWaitEventSupportProcess();
		return;
	}

	/* FIXME: this is called very early at startup, before GUCs are processed */
	n = MaxLivePostmasterChildren();
	//n = 300;
	thread_wakeup_readfds = MemoryContextAlloc(MultiThreadGlobalContext, (n + 1) * sizeof(int));
	thread_wakeup_writefds = MemoryContextAlloc(MultiThreadGlobalContext, (n + 1) * sizeof(int));
	for (int i = 0; i < n + 1; i++)
	{
		int			pipefd[2];

		/*
		 * Set up the self-pipe that allows a signal handler to wake up the
		 * poll()/epoll_wait() in WaitInterrupt. Make the write-end
		 * non-blocking, so that SendInterrupt won't block if the event has
		 * already been set many times filling the kernel buffer. Make the
		 * read-end non-blocking too, so that we can easily clear the pipe by
		 * reading until EAGAIN or EWOULDBLOCK.  Also, make both FDs
		 * close-on-exec, since we surely do not want any child processes
		 * messing with them.
		 */
		if (pipe(pipefd) < 0)
			elog(FATAL, "pipe() failed: %m");
		if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) == -1)
			elog(FATAL, "fcntl(F_SETFL) failed on read-end of thread wakeup pipe: %m");
		if (fcntl(pipefd[1], F_SETFL, O_NONBLOCK) == -1)
			elog(FATAL, "fcntl(F_SETFL) failed on write-end of thread wakeup pipe: %m");
		if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) == -1)
			elog(FATAL, "fcntl(F_SETFD) failed on read-end of thread wakeup pipe: %m");
		if (fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) == -1)
			elog(FATAL, "fcntl(F_SETFD) failed on write-end of thread wakeup pipe: %m");

		thread_wakeup_readfds[i] = pipefd[0];
		thread_wakeup_writefds[i] = pipefd[1];
	}
}


static void
InitializeWaitEventSupportProcess(void)
{
#if defined(WAIT_USE_SELF_PIPE)
	int			pipefd[2];

	if (IsUnderPostmaster)
	{
		/*
		 * We might have inherited connections to a self-pipe created by the
		 * postmaster.  It's critical that child processes create their own
		 * self-pipes, of course, and we really want them to close the
		 * inherited FDs for safety's sake.
		 */
		if (selfpipe_owner_pid != 0)
		{
			/* Assert we go through here but once in a child process */
			Assert(selfpipe_owner_pid != MyProcPid);
			/* Release postmaster's pipe FDs; ignore any error */
			(void) close(selfpipe_readfd);
			(void) close(selfpipe_writefd);
			/* Clean up, just for safety's sake; we'll set these below */
			selfpipe_readfd = selfpipe_writefd = -1;
			selfpipe_owner_pid = 0;
			/* Keep fd.c's accounting straight */
			ReleaseExternalFD();
			ReleaseExternalFD();
		}
		else
		{
			/*
			 * Postmaster didn't create a self-pipe ... or else we're in an
			 * EXEC_BACKEND build, in which case it doesn't matter since the
			 * postmaster's pipe FDs were closed by the action of FD_CLOEXEC.
			 * fd.c won't have state to clean up, either.
			 */
			Assert(selfpipe_readfd == -1);
		}
	}
	else
	{
		/* In postmaster or standalone backend, assert we do this but once */
		Assert(selfpipe_readfd == -1);
		Assert(selfpipe_owner_pid == 0);
	}

	/*
	 * Set up the self-pipe that allows a signal handler to wake up the
	 * poll()/epoll_wait() in WaitInterrupt. Make the write-end non-blocking,
	 * so that SendInterrupt won't block if the event has already been set
	 * many times filling the kernel buffer. Make the read-end non-blocking
	 * too, so that we can easily clear the pipe by reading until EAGAIN or
	 * EWOULDBLOCK. Also, make both FDs close-on-exec, since we surely do not
	 * want any child processes messing with them.
	 */
	if (pipe(pipefd) < 0)
		elog(FATAL, "pipe() failed: %m");
	if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) == -1)
		elog(FATAL, "fcntl(F_SETFL) failed on read-end of self-pipe: %m");
	if (fcntl(pipefd[1], F_SETFL, O_NONBLOCK) == -1)
		elog(FATAL, "fcntl(F_SETFL) failed on write-end of self-pipe: %m");
	if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) == -1)
		elog(FATAL, "fcntl(F_SETFD) failed on read-end of self-pipe: %m");
	if (fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) == -1)
		elog(FATAL, "fcntl(F_SETFD) failed on write-end of self-pipe: %m");

	selfpipe_readfd = pipefd[0];
	selfpipe_writefd = pipefd[1];
	selfpipe_owner_pid = MyProcPid;

	/* Tell fd.c about these two long-lived FDs */
	ReserveExternalFD();
	ReserveExternalFD();

	pqsignal(SIGURG, interrupt_sigurg_handler);
#endif

#ifdef WAIT_USE_SIGNALFD
	sigset_t	signalfd_mask;

	if (IsUnderPostmaster)
	{
		/*
		 * It would probably be safe to re-use the inherited signalfd since
		 * signalfds only see the current process's pending signals, but it
		 * seems less surprising to close it and create our own.
		 */
		if (signal_fd != -1)
		{
			/* Release postmaster's signal FD; ignore any error */
			(void) close(signal_fd);
			signal_fd = -1;
			ReleaseExternalFD();
		}
	}

	/* Block SIGURG, because we'll receive it through a signalfd. */
	sigaddset(&UnBlockSig, SIGURG);

	/* Set up the signalfd to receive SIGURG notifications. */
	sigemptyset(&signalfd_mask);
	sigaddset(&signalfd_mask, SIGURG);
	signal_fd = signalfd(-1, &signalfd_mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (signal_fd < 0)
		elog(FATAL, "signalfd() failed");
	ReserveExternalFD();
#endif

#ifdef WAIT_USE_KQUEUE
	/* Ignore SIGURG, because we'll receive it via kqueue. */
	pqsignal(SIGURG, SIG_IGN);
#endif

#ifdef WAIT_USE_WIN32
	LocalInterruptWakeupEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (LocalInterruptWakeupEvent == NULL)
		elog(ERROR, "CreateEvent failed: error code %lu", GetLastError());
#endif
}

void
InitializeInterruptWaitSet(void)
{
	int			interrupt_pos PG_USED_FOR_ASSERTS_ONLY;

	Assert(InterruptWaitSet == NULL);

	/* Set up the WaitEventSet used by WaitInterrupt(). */
	InterruptWaitSet = CreateWaitEventSet(NULL, 2);
	interrupt_pos = AddWaitEventToSet(InterruptWaitSet, WL_INTERRUPT, PGINVALID_SOCKET,
									  0, NULL);
	if (IsUnderPostmaster)
		AddWaitEventToSet(InterruptWaitSet, WL_EXIT_ON_PM_DEATH,
						  PGINVALID_SOCKET, 0, NULL);

	Assert(interrupt_pos == InterruptWaitSetInterruptPos);
}

void
ShutdownWaitEventSupport(void)
{
#if defined(WAIT_USE_POLL)
	pqsignal(SIGURG, SIG_IGN);
#endif

	if (InterruptWaitSet)
	{
		FreeWaitEventSet(InterruptWaitSet);
		InterruptWaitSet = NULL;
	}

#if defined(WAIT_USE_SELF_PIPE)
	close(selfpipe_readfd);
	close(selfpipe_writefd);
	selfpipe_readfd = -1;
	selfpipe_writefd = -1;
	selfpipe_owner_pid = InvalidPid;
#endif

#if defined(WAIT_USE_SIGNALFD)
	close(signal_fd);
	signal_fd = -1;
#endif
}

/*
 * Wait for any of the interrupts in interruptMask to be set, or for
 * postmaster death, or until timeout is exceeded. 'wakeEvents' is a bitmask
 * that specifies which of those events to wait for. If the interrupt is
 * already pending (and WL_INTERRUPT is given), the function returns
 * immediately.
 *
 * The "timeout" is given in milliseconds. It must be >= 0 if WL_TIMEOUT flag
 * is given.  Although it is declared as "long", we don't actually support
 * timeouts longer than INT_MAX milliseconds.  Note that some extra overhead
 * is incurred when WL_TIMEOUT is given, so avoid using a timeout if possible.
 *
 * Returns bit mask indicating which condition(s) caused the wake-up. Note
 * that if multiple wake-up conditions are true, there is no guarantee that
 * we return all of them in one call, but we will return at least one.
 */
int
WaitInterrupt(uint32 interruptMask, int wakeEvents, long timeout,
			  uint32 wait_event_info)
{
	WaitEvent	event;

	/* Postmaster-managed callers must handle postmaster death somehow. */
	Assert(!IsUnderPostmaster ||
		   (wakeEvents & WL_EXIT_ON_PM_DEATH) ||
		   (wakeEvents & WL_POSTMASTER_DEATH));

	/*
	 * Some callers may have an interrupt mask different from last time, or no
	 * interrupt mask at all, or want to handle postmaster death differently.
	 * It's cheap to assign those, so just do it every time.
	 */
	if (!(wakeEvents & WL_INTERRUPT))
		interruptMask = 0;
	ModifyWaitEvent(InterruptWaitSet, InterruptWaitSetInterruptPos, WL_INTERRUPT, interruptMask);
	InterruptWaitSet->exit_on_postmaster_death =
		((wakeEvents & WL_EXIT_ON_PM_DEATH) != 0);

	if (WaitEventSetWait(InterruptWaitSet,
						 (wakeEvents & WL_TIMEOUT) ? timeout : -1,
						 &event, 1,
						 wait_event_info) == 0)
		return WL_TIMEOUT;
	else
		return event.events;
}

/*
 * Like WaitInterrupt, but with an extra socket argument for WL_SOCKET_*
 * conditions.
 *
 * When waiting on a socket, EOF and error conditions always cause the socket
 * to be reported as readable/writable/connected, so that the caller can deal
 * with the condition.
 *
 * wakeEvents must include either WL_EXIT_ON_PM_DEATH for automatic exit
 * if the postmaster dies or WL_POSTMASTER_DEATH for a flag set in the
 * return value if the postmaster dies.  The latter is useful for rare cases
 * where some behavior other than immediate exit is needed.
 *
 * NB: These days this is just a wrapper around the WaitEventSet API. When
 * using an interrupt very frequently, consider creating a longer living
 * WaitEventSet instead; that's more efficient.
 */
int
WaitInterruptOrSocket(uint32 interruptMask, int wakeEvents, pgsocket sock,
					  long timeout, uint32 wait_event_info)
{
	int			ret = 0;
	int			rc;
	WaitEvent	event;
	WaitEventSet *set = CreateWaitEventSet(CurrentResourceOwner, 3);

	if (wakeEvents & WL_TIMEOUT)
		Assert(timeout >= 0);
	else
		timeout = -1;

	if (wakeEvents & WL_INTERRUPT)
		AddWaitEventToSet(set, WL_INTERRUPT, PGINVALID_SOCKET,
						  interruptMask, NULL);

	/* Postmaster-managed callers must handle postmaster death somehow. */
	Assert(!IsUnderPostmaster ||
		   (wakeEvents & WL_EXIT_ON_PM_DEATH) ||
		   (wakeEvents & WL_POSTMASTER_DEATH));

	if ((wakeEvents & WL_POSTMASTER_DEATH) && IsUnderPostmaster)
		AddWaitEventToSet(set, WL_POSTMASTER_DEATH, PGINVALID_SOCKET,
						  0, NULL);

	if ((wakeEvents & WL_EXIT_ON_PM_DEATH) && IsUnderPostmaster)
		AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
						  0, NULL);

	if (wakeEvents & WL_SOCKET_MASK)
	{
		int			ev;

		ev = wakeEvents & WL_SOCKET_MASK;
		AddWaitEventToSet(set, ev, sock, 0, NULL);
	}

	rc = WaitEventSetWait(set, timeout, &event, 1, wait_event_info);

	if (rc == 0)
		ret |= WL_TIMEOUT;
	else
	{
		ret |= event.events & (WL_INTERRUPT |
							   WL_POSTMASTER_DEATH |
							   WL_SOCKET_MASK);
	}

	FreeWaitEventSet(set);

	return ret;
}

/*
 * Wake up my process if it's currently waiting.
 *
 * NB: be sure to save and restore errno around it.  (That's standard practice
 * in most signal handlers, of course, but we used to omit it in handlers that
 * only set a flag.) XXX
 *
 * NB: this function is called from critical sections and signal handlers so
 * throwing an error is not a good idea.
 */
void
WakeupMyProc(void)
{
#ifndef WIN32
#if defined(WAIT_USE_SELF_PIPE)
	if (waiting)
		sendSelfPipeByte();
#else
	if (waiting)
		kill(MyProcPid, SIGURG);
#endif
#else
	SetEvent(MyProc ? MyProc->interruptWakeupEvent : LocalInterruptWakeupEvent);
#endif
}

/*
 * Wake up another process if it's currently waiting.
 */
void
WakeupOtherProc(PGPROC *proc)
{
	/*
	 * Note: This can also be called from the postmaster, so be careful to not
	 * assume that the contents of shared memory are valid.  Reading the 'pid'
	 * (or event handle on Windows) is safe enough.
	 */
	if (IsMultiThreaded)
	{
		int			owner_pmchild = proc->pmchild;

		if (owner_pmchild == 0)
			return;
		else if (owner_pmchild == (IsUnderPostmaster ? MyPMChildSlot : POSTMASTER_PMCHILD_SLOT))
		{
			if (waiting)
				sendPipeWakeupByte(thread_wakeup_writefds[(owner_pmchild == POSTMASTER_PMCHILD_SLOT) ? 0 : owner_pmchild]);
		}
		else
			sendPipeWakeupByte(thread_wakeup_writefds[(owner_pmchild == POSTMASTER_PMCHILD_SLOT) ? 0 : owner_pmchild]);
	}



#ifndef WIN32
	kill(proc->pid, SIGURG);
#else
	SetEvent(proc->interruptWakeupEvent);

	/*
	 * Note that we silently ignore any errors. We might be in a signal
	 * handler or other critical path where it's not safe to call elog().
	 */
#endif
}

/*
 * Create a WaitEventSet with space for nevents different events to wait for.
 *
 * These events can then be efficiently waited upon together, using
 * WaitEventSetWait().
 *
 * The WaitEventSet is tracked by the given 'resowner'.  Use NULL for session
 * lifetime.
 */
WaitEventSet *
CreateWaitEventSet(ResourceOwner resowner, int nevents)
{
	WaitEventSet *set;
	char	   *data;
	Size		sz = 0;

	/*
	 * Use MAXALIGN size/alignment to guarantee that later uses of memory are
	 * aligned correctly. E.g. epoll_event might need 8 byte alignment on some
	 * platforms, but earlier allocations like WaitEventSet and WaitEvent
	 * might not be sized to guarantee that when purely using sizeof().
	 */
	sz += MAXALIGN(sizeof(WaitEventSet));
	sz += MAXALIGN(sizeof(WaitEvent) * nevents);

#if defined(WAIT_USE_EPOLL)
	sz += MAXALIGN(sizeof(struct epoll_event) * nevents);
#elif defined(WAIT_USE_KQUEUE)
	sz += MAXALIGN(sizeof(struct kevent) * nevents);
#elif defined(WAIT_USE_POLL)
	sz += MAXALIGN(sizeof(struct pollfd) * nevents);
#elif defined(WAIT_USE_WIN32)
	/* need space for the pgwin32_signal_event */
	sz += MAXALIGN(sizeof(HANDLE) * (nevents + 1));
#endif

	if (resowner != NULL)
		ResourceOwnerEnlarge(resowner);

	data = (char *) MemoryContextAllocZero(TopMemoryContext, sz);

	set = (WaitEventSet *) data;
	data += MAXALIGN(sizeof(WaitEventSet));

	set->events = (WaitEvent *) data;
	data += MAXALIGN(sizeof(WaitEvent) * nevents);

#if defined(WAIT_USE_EPOLL)
	set->epoll_ret_events = (struct epoll_event *) data;
	data += MAXALIGN(sizeof(struct epoll_event) * nevents);
#elif defined(WAIT_USE_KQUEUE)
	set->kqueue_ret_events = (struct kevent *) data;
	data += MAXALIGN(sizeof(struct kevent) * nevents);
#elif defined(WAIT_USE_POLL)
	set->pollfds = (struct pollfd *) data;
	data += MAXALIGN(sizeof(struct pollfd) * nevents);
#elif defined(WAIT_USE_WIN32)
	set->handles = (HANDLE) data;
	data += MAXALIGN(sizeof(HANDLE) * nevents);
#endif

	set->interrupt_mask = 0;
	set->interrupt_pos = -1;
	set->nevents_space = nevents;
	set->exit_on_postmaster_death = false;

	if (resowner != NULL)
	{
		ResourceOwnerRememberWaitEventSet(resowner, set);
		set->owner = resowner;
	}

#if defined(WAIT_USE_EPOLL)
	if (!AcquireExternalFD())
	{
		/* treat this as though epoll_create1 itself returned EMFILE */
		elog(ERROR, "epoll_create1 failed: %m");
	}
	set->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (set->epoll_fd < 0)
	{
		ReleaseExternalFD();
		elog(ERROR, "epoll_create1 failed: %m");
	}
#elif defined(WAIT_USE_KQUEUE)
	if (!AcquireExternalFD())
	{
		/* treat this as though kqueue itself returned EMFILE */
		elog(ERROR, "kqueue failed: %m");
	}
	set->kqueue_fd = kqueue();
	if (set->kqueue_fd < 0)
	{
		ReleaseExternalFD();
		elog(ERROR, "kqueue failed: %m");
	}
	if (fcntl(set->kqueue_fd, F_SETFD, FD_CLOEXEC) == -1)
	{
		int			save_errno = errno;

		close(set->kqueue_fd);
		ReleaseExternalFD();
		errno = save_errno;
		elog(ERROR, "fcntl(F_SETFD) failed on kqueue descriptor: %m");
	}
	set->report_postmaster_not_running = false;
#elif defined(WAIT_USE_WIN32)

	/*
	 * To handle signals while waiting, we need to add a win32 specific event.
	 * We accounted for the additional event at the top of this routine. See
	 * port/win32/signal.c for more details.
	 *
	 * Note: pgwin32_signal_event should be first to ensure that it will be
	 * reported when multiple events are set.  We want to guarantee that
	 * pending signals are serviced.
	 */
	set->handles[0] = pgwin32_signal_event;
	StaticAssertStmt(WSA_INVALID_EVENT == NULL, "");
#endif

	return set;
}

/*
 * Free a previously created WaitEventSet.
 *
 * Note: preferably, this shouldn't have to free any resources that could be
 * inherited across an exec().  If it did, we'd likely leak those resources in
 * many scenarios.  For the epoll case, we ensure that by setting EPOLL_CLOEXEC
 * when the FD is created.  For the Windows case, we assume that the handles
 * involved are non-inheritable.
 */
void
FreeWaitEventSet(WaitEventSet *set)
{
	if (set->owner)
	{
		ResourceOwnerForgetWaitEventSet(set->owner, set);
		set->owner = NULL;
	}

#if defined(WAIT_USE_EPOLL)
	close(set->epoll_fd);
	ReleaseExternalFD();
#elif defined(WAIT_USE_KQUEUE)
	close(set->kqueue_fd);
	ReleaseExternalFD();
#elif defined(WAIT_USE_WIN32)
	for (WaitEvent *cur_event = set->events;
		 cur_event < (set->events + set->nevents);
		 cur_event++)
	{
		if (cur_event->events & WL_INTERRUPT)
		{
			/* uses the process's wakeup HANDLE */
		}
		else if (cur_event->events & WL_POSTMASTER_DEATH)
		{
			/* uses PostmasterHandle */
		}
		else
		{
			/* Clean up the event object we created for the socket */
			WSAEventSelect(cur_event->fd, NULL, 0);
			WSACloseEvent(set->handles[cur_event->pos + 1]);
		}
	}
#endif

	pfree(set);
}

/*
 * Free a previously created WaitEventSet in a child process after a fork().
 */
void
FreeWaitEventSetAfterFork(WaitEventSet *set)
{
#if defined(WAIT_USE_EPOLL)
	close(set->epoll_fd);
	ReleaseExternalFD();
#elif defined(WAIT_USE_KQUEUE)
	/* kqueues are not normally inherited by child processes */
	ReleaseExternalFD();
#endif

	pfree(set);
}

/* ---
 * Add an event to the set. Possible events are:
 * - WL_INTERRUPT: Wait for the interrupt to be set
 * - WL_POSTMASTER_DEATH: Wait for postmaster to die
 * - WL_SOCKET_READABLE: Wait for socket to become readable,
 *	 can be combined in one event with other WL_SOCKET_* events
 * - WL_SOCKET_WRITEABLE: Wait for socket to become writeable,
 *	 can be combined with other WL_SOCKET_* events
 * - WL_SOCKET_CONNECTED: Wait for socket connection to be established,
 *	 can be combined with other WL_SOCKET_* events (on non-Windows
 *	 platforms, this is the same as WL_SOCKET_WRITEABLE)
 * - WL_SOCKET_ACCEPT: Wait for new connection to a server socket,
 *	 can be combined with other WL_SOCKET_* events (on non-Windows
 *	 platforms, this is the same as WL_SOCKET_READABLE)
 * - WL_SOCKET_CLOSED: Wait for socket to be closed by remote peer.
 * - WL_EXIT_ON_PM_DEATH: Exit immediately if the postmaster dies
 *
 * Returns the offset in WaitEventSet->events (starting from 0), which can be
 * used to modify previously added wait events using ModifyWaitEvent().
 *
 * In the WL_SOCKET_READABLE/WRITEABLE/CONNECTED/ACCEPT cases, EOF and error
 * conditions cause the socket to be reported as readable/writable/connected,
 * so that the caller can deal with the condition.
 *
 * The user_data pointer specified here will be set for the events returned
 * by WaitEventSetWait(), allowing to easily associate additional data with
 * events.
 */
int
AddWaitEventToSet(WaitEventSet *set, uint32 events, pgsocket fd, uint32 interruptMask,
				  void *user_data)
{
	WaitEvent  *event;

	/* not enough space */
	Assert(set->nevents < set->nevents_space);

	if (events == WL_EXIT_ON_PM_DEATH)
	{
		events = WL_POSTMASTER_DEATH;
		set->exit_on_postmaster_death = true;
	}

	/*
	 * It doesn't make much sense to wait for WL_INTERRUPT with empty
	 * interruptMask, but we allow it so that you can use ModifyEvent to set
	 * the interruptMask later. Non-zero interruptMask doesn't make sense
	 * without WL_INTERRUPT, however.
	 */
	if (interruptMask != 0)
	{
		if ((events & WL_INTERRUPT) != WL_INTERRUPT)
			elog(ERROR, "interrupted events only support being set");
	}

	/* waiting for socket readiness without a socket indicates a bug */
	if (fd == PGINVALID_SOCKET && (events & WL_SOCKET_MASK))
		elog(ERROR, "cannot wait on socket event without a socket");

	event = &set->events[set->nevents];
	event->pos = set->nevents++;
	event->fd = fd;
	event->events = events;
	event->user_data = user_data;
#ifdef WIN32
	event->reset = false;
#endif

	if (events == WL_INTERRUPT)
	{
		set->interrupt_mask = interruptMask;
		set->interrupt_pos = event->pos;
#if defined(WAIT_USE_SELF_PIPE)
		event->fd = selfpipe_readfd;
#elif defined(WAIT_USE_SIGNALFD)
		event->fd = signal_fd;
#else
		event->fd = PGINVALID_SOCKET;
#ifdef WAIT_USE_EPOLL
		return event->pos;
#endif
#endif
	}
	else if (events == WL_POSTMASTER_DEATH)
	{
#ifndef WIN32
		event->fd = postmaster_alive_fds[POSTMASTER_FD_WATCH];
#endif
	}

	/* perform wait primitive specific initialization, if needed */
#if defined(WAIT_USE_EPOLL)
	WaitEventAdjustEpoll(set, event, EPOLL_CTL_ADD);
#elif defined(WAIT_USE_KQUEUE)
	WaitEventAdjustKqueue(set, event, 0);
#elif defined(WAIT_USE_POLL)
	WaitEventAdjustPoll(set, event);
#elif defined(WAIT_USE_WIN32)
	WaitEventAdjustWin32(set, event);
#endif

	return event->pos;
}

/*
 * Change the event mask and, in the WL_INTERRUPT case, the interrupt mask
 * associated with the WaitEvent.  The interrupt mask may be changed to 0 to
 * disable the wakeups on interrupts temporarily, and then set back later.
 *
 * 'pos' is the id returned by AddWaitEventToSet.
 */
void
ModifyWaitEvent(WaitEventSet *set, int pos, uint32 events, uint32 interruptMask)
{
	WaitEvent  *event;
#if defined(WAIT_USE_KQUEUE)
	int			old_events;
#endif

	Assert(pos < set->nevents);

	event = &set->events[pos];
#if defined(WAIT_USE_KQUEUE)
	old_events = event->events;
#endif

	/*
	 * If neither the event mask nor the associated interrupt mask changes,
	 * return early. That's an important optimization for some sockets, where
	 * ModifyWaitEvent is frequently used to switch from waiting for reads to
	 * waiting on writes.
	 */
	if (events == event->events &&
		(!(event->events & WL_INTERRUPT) || set->interrupt_mask == interruptMask))
		return;

	if (event->events & WL_INTERRUPT &&
		events != event->events)
	{
		elog(ERROR, "cannot modify interrupts event");
	}

	if (event->events & WL_POSTMASTER_DEATH)
	{
		elog(ERROR, "cannot modify postmaster death event");
	}

	/* FIXME: validate event mask */
	event->events = events;

	if (events == WL_INTERRUPT)
	{
		set->interrupt_mask = interruptMask;

		/*
		 * We don't bother to adjust the event when the interrupt mask
		 * changes.  It means that when interrupt mask is set to 0, we will
		 * listen on the kernel object unnecessarily, and might get some
		 * spurious wakeups. The interrupt sending code should not wakes us up
		 * if the maybeSleepingOnInterrupts is zero, though, so it should be
		 * rare.
		 */
		return;
	}

#if defined(WAIT_USE_EPOLL)
	WaitEventAdjustEpoll(set, event, EPOLL_CTL_MOD);
#elif defined(WAIT_USE_KQUEUE)
	WaitEventAdjustKqueue(set, event, old_events);
#elif defined(WAIT_USE_POLL)
	WaitEventAdjustPoll(set, event);
#elif defined(WAIT_USE_WIN32)
	WaitEventAdjustWin32(set, event);
#endif
}

#if defined(WAIT_USE_EPOLL)
/*
 * action can be one of EPOLL_CTL_ADD | EPOLL_CTL_MOD | EPOLL_CTL_DEL
 */
static void
WaitEventAdjustEpoll(WaitEventSet *set, WaitEvent *event, int action)
{
	struct epoll_event epoll_ev;
	int			rc;

	/* pointer to our event, returned by epoll_wait */
	epoll_ev.data.ptr = event;
	/* always wait for errors */
	epoll_ev.events = EPOLLERR | EPOLLHUP;

	/* prepare pollfd entry once */
	if (event->events == WL_INTERRUPT)
	{
		epoll_ev.events |= EPOLLIN;
	}
	else if (event->events == WL_POSTMASTER_DEATH)
	{
		epoll_ev.events |= EPOLLIN;
	}
	else
	{
		Assert(event->fd != PGINVALID_SOCKET);
		Assert(event->events & (WL_SOCKET_READABLE |
								WL_SOCKET_WRITEABLE |
								WL_SOCKET_CLOSED));

		if (event->events & WL_SOCKET_READABLE)
			epoll_ev.events |= EPOLLIN;
		if (event->events & WL_SOCKET_WRITEABLE)
			epoll_ev.events |= EPOLLOUT;
		if (event->events & WL_SOCKET_CLOSED)
			epoll_ev.events |= EPOLLRDHUP;
	}

	/*
	 * Even though unused, we also pass epoll_ev as the data argument if
	 * EPOLL_CTL_DEL is passed as action.  There used to be an epoll bug
	 * requiring that, and actually it makes the code simpler...
	 */
	rc = epoll_ctl(set->epoll_fd, action, event->fd, &epoll_ev);

	if (rc < 0)
		ereport(ERROR,
				(errcode_for_socket_access(),
				 errmsg("%s() failed: %m",
						"epoll_ctl")));
}
#endif

#if defined(WAIT_USE_POLL)
static void
WaitEventAdjustPoll(WaitEventSet *set, WaitEvent *event)
{
	struct pollfd *pollfd = &set->pollfds[event->pos];

	pollfd->revents = 0;
	pollfd->fd = event->fd;

	/* prepare pollfd entry once */
	if (event->events == WL_INTERRUPT)
	{
		pollfd->events = POLLIN;
	}
	else if (event->events == WL_POSTMASTER_DEATH)
	{
		pollfd->events = POLLIN;
	}
	else
	{
		Assert(event->events & (WL_SOCKET_READABLE |
								WL_SOCKET_WRITEABLE |
								WL_SOCKET_CLOSED));
		pollfd->events = 0;
		if (event->events & WL_SOCKET_READABLE)
			pollfd->events |= POLLIN;
		if (event->events & WL_SOCKET_WRITEABLE)
			pollfd->events |= POLLOUT;
#ifdef POLLRDHUP
		if (event->events & WL_SOCKET_CLOSED)
			pollfd->events |= POLLRDHUP;
#endif
	}

	Assert(event->fd != PGINVALID_SOCKET);
}
#endif

#if defined(WAIT_USE_KQUEUE)

/*
 * On most BSD family systems, the udata member of struct kevent is of type
 * void *, so we could directly convert to/from WaitEvent *.  Unfortunately,
 * NetBSD has it as intptr_t, so here we wallpaper over that difference with
 * an lvalue cast.
 */
#define AccessWaitEvent(k_ev) (*((WaitEvent **)(&(k_ev)->udata)))

static inline void
WaitEventAdjustKqueueAdd(struct kevent *k_ev, int filter, int action,
						 WaitEvent *event)
{
	k_ev->ident = event->fd;
	k_ev->filter = filter;
	k_ev->flags = action;
	k_ev->fflags = 0;
	k_ev->data = 0;
	AccessWaitEvent(k_ev) = event;
}

static inline void
WaitEventAdjustKqueueAddPostmaster(struct kevent *k_ev, WaitEvent *event)
{
	/* For now postmaster death can only be added, not removed. */
	k_ev->ident = PostmasterPid;
	k_ev->filter = EVFILT_PROC;
	k_ev->flags = EV_ADD;
	k_ev->fflags = NOTE_EXIT;
	k_ev->data = 0;
	AccessWaitEvent(k_ev) = event;
}

static inline void
WaitEventAdjustKqueueAddInterruptWakeup(struct kevent *k_ev, WaitEvent *event)
{
	/* For now interrupt wakeup can only be added, not removed. */
	k_ev->ident = SIGURG;
	k_ev->filter = EVFILT_SIGNAL;
	k_ev->flags = EV_ADD;
	k_ev->fflags = 0;
	k_ev->data = 0;
	AccessWaitEvent(k_ev) = event;
}

/*
 * old_events is the previous event mask, used to compute what has changed.
 */
static void
WaitEventAdjustKqueue(WaitEventSet *set, WaitEvent *event, int old_events)
{
	int			rc;
	struct kevent k_ev[2];
	int			count = 0;
	bool		new_filt_read = false;
	bool		old_filt_read = false;
	bool		new_filt_write = false;
	bool		old_filt_write = false;

	if (old_events == event->events)
		return;

	Assert(event->events == WL_INTERRUPT ||
		   event->events == WL_POSTMASTER_DEATH ||
		   (event->events & (WL_SOCKET_READABLE |
							 WL_SOCKET_WRITEABLE |
							 WL_SOCKET_CLOSED)));

	if (event->events == WL_POSTMASTER_DEATH)
	{
		/*
		 * Unlike all the other implementations, we detect postmaster death
		 * using process notification instead of waiting on the postmaster
		 * alive pipe.
		 */
		WaitEventAdjustKqueueAddPostmaster(&k_ev[count++], event);
	}
	else if (event->events == WL_INTERRUPT)
	{
		/* We detect interrupt wakeup using a signal event. */
		WaitEventAdjustKqueueAddInterruptWakeup(&k_ev[count++], event);
	}
	else
	{
		/*
		 * We need to compute the adds and deletes required to get from the
		 * old event mask to the new event mask, since kevent treats readable
		 * and writable as separate events.
		 */
		if (old_events & (WL_SOCKET_READABLE | WL_SOCKET_CLOSED))
			old_filt_read = true;
		if (event->events & (WL_SOCKET_READABLE | WL_SOCKET_CLOSED))
			new_filt_read = true;
		if (old_events & WL_SOCKET_WRITEABLE)
			old_filt_write = true;
		if (event->events & WL_SOCKET_WRITEABLE)
			new_filt_write = true;
		if (old_filt_read && !new_filt_read)
			WaitEventAdjustKqueueAdd(&k_ev[count++], EVFILT_READ, EV_DELETE,
									 event);
		else if (!old_filt_read && new_filt_read)
			WaitEventAdjustKqueueAdd(&k_ev[count++], EVFILT_READ, EV_ADD,
									 event);
		if (old_filt_write && !new_filt_write)
			WaitEventAdjustKqueueAdd(&k_ev[count++], EVFILT_WRITE, EV_DELETE,
									 event);
		else if (!old_filt_write && new_filt_write)
			WaitEventAdjustKqueueAdd(&k_ev[count++], EVFILT_WRITE, EV_ADD,
									 event);
	}

	/* For WL_SOCKET_READ -> WL_SOCKET_CLOSED, no change needed. */
	if (count == 0)
		return;

	Assert(count <= 2);

	rc = kevent(set->kqueue_fd, &k_ev[0], count, NULL, 0, NULL);

	/*
	 * When adding the postmaster's pid, we have to consider that it might
	 * already have exited and perhaps even been replaced by another process
	 * with the same pid.  If so, we have to defer reporting this as an event
	 * until the next call to WaitEventSetWaitBlock().
	 */

	if (rc < 0)
	{
		if (event->events == WL_POSTMASTER_DEATH &&
			(errno == ESRCH || errno == EACCES))
			set->report_postmaster_not_running = true;
		else
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("%s() failed: %m",
							"kevent")));
	}
	else if (event->events == WL_POSTMASTER_DEATH &&
			 PostmasterPid != getppid() &&
			 !PostmasterIsAlive())
	{
		/*
		 * The extra PostmasterIsAliveInternal() check prevents false alarms
		 * on systems that give a different value for getppid() while being
		 * traced by a debugger.
		 */
		set->report_postmaster_not_running = true;
	}
}

#endif

#if defined(WAIT_USE_WIN32)
static void
WaitEventAdjustWin32(WaitEventSet *set, WaitEvent *event)
{
	HANDLE	   *handle = &set->handles[event->pos + 1];

	if (event->events == WL_INTERRUPT)
	{
		/*
		 * We register the event even if interrupt_mask is zero. We might get
		 * some spurious wakeups, but that's OK
		 */
		*handle = MyProc ? MyProc->interruptWakeupEvent : LocalInterruptWakeupEvent;
	}
	else if (event->events == WL_POSTMASTER_DEATH)
	{
		*handle = PostmasterHandle;
	}
	else
	{
		int			flags = FD_CLOSE;	/* always check for errors/EOF */

		if (event->events & WL_SOCKET_READABLE)
			flags |= FD_READ;
		if (event->events & WL_SOCKET_WRITEABLE)
			flags |= FD_WRITE;
		if (event->events & WL_SOCKET_CONNECTED)
			flags |= FD_CONNECT;
		if (event->events & WL_SOCKET_ACCEPT)
			flags |= FD_ACCEPT;

		if (*handle == WSA_INVALID_EVENT)
		{
			*handle = WSACreateEvent();
			if (*handle == WSA_INVALID_EVENT)
				elog(ERROR, "failed to create event for socket: error code %d",
					 WSAGetLastError());
		}
		if (WSAEventSelect(event->fd, *handle, flags) != 0)
			elog(ERROR, "failed to set up event for socket: error code %d",
				 WSAGetLastError());

		Assert(event->fd != PGINVALID_SOCKET);
	}
}
#endif

/*
 * Wait for events added to the set to happen, or until the timeout is
 * reached.  At most nevents occurred events are returned.
 *
 * If timeout = -1, block until an event occurs; if 0, check sockets for
 * readiness, but don't block; if > 0, block for at most timeout milliseconds.
 *
 * Returns the number of events occurred, or 0 if the timeout was reached.
 *
 * Returned events will have the fd, pos, user_data fields set to the
 * values associated with the registered event.
 */
int
WaitEventSetWait(WaitEventSet *set, long timeout,
				 WaitEvent *occurred_events, int nevents,
				 uint32 wait_event_info)
{
	int			returned_events = 0;
	instr_time	start_time;
	instr_time	cur_time;
	long		cur_timeout = -1;

	Assert(nevents > 0);

	/*
	 * Initialize timeout if requested.  We must record the current time so
	 * that we can determine the remaining timeout if interrupted.
	 */
	if (timeout >= 0)
	{
		INSTR_TIME_SET_CURRENT(start_time);
		Assert(timeout >= 0 && timeout <= INT_MAX);
		cur_timeout = timeout;
	}
	else
		INSTR_TIME_SET_ZERO(start_time);

	pgstat_report_wait_start(wait_event_info);

#ifndef WIN32
	waiting = true;
#else
	/* Ensure that signals are serviced even if interrupt is already pending */
	pgwin32_dispatch_queued_signals();
#endif
	while (returned_events == 0)
	{
		int			rc;

		/*
		 * Check if the interrupt is already pending. If so, leave the loop
		 * immediately, avoid blocking again. We don't attempt to report any
		 * other events that might also be satisfied.
		 *
		 * If someone sets the interrupt flag between this and the
		 * WaitEventSetWaitBlock() below, the setter will write a byte to the
		 * pipe (or signal us and the signal handler will do that), and the
		 * readiness routine will return immediately.
		 *
		 * On unix, If there's a pending byte in the self pipe, we'll notice
		 * whenever blocking. Only clearing the pipe in that case avoids
		 * having to drain it every time WaitInterruptOrSocket() is used.
		 * Should the pipe-buffer fill up we're still ok, because the pipe is
		 * in nonblocking mode. It's unlikely for that to happen, because the
		 * self pipe isn't filled unless we're blocking (waiting = true), or
		 * from inside a signal handler in interrupt_sigurg_handler().
		 *
		 * On windows, we'll also notice if there's a pending event for the
		 * interrupt when blocking, but there's no danger of anything filling
		 * up, as "Setting an event that is already set has no effect.".
		 *
		 * Note: we assume that the kernel calls involved in interrupt
		 * management will provide adequate synchronization on machines with
		 * weak memory ordering, so that we cannot miss seeing is_set if a
		 * notification has already been queued.
		 */
		if (set->interrupt_mask != 0 && (pg_atomic_read_u32(MyPendingInterrupts) & set->interrupt_mask) == 0)
		{
			/* about to sleep waiting for interrupts */
			pg_atomic_write_u32(MyMaybeSleepingOnInterrupts, set->interrupt_mask);
			pg_memory_barrier();
			/* and recheck */
		}

		if (set->interrupt_mask != 0 && (pg_atomic_read_u32(MyPendingInterrupts) & set->interrupt_mask) != 0)
		{
			occurred_events->fd = PGINVALID_SOCKET;
			occurred_events->pos = set->interrupt_pos;
			occurred_events->user_data =
				set->events[set->interrupt_pos].user_data;
			occurred_events->events = WL_INTERRUPT;
			occurred_events++;
			returned_events++;

			/* could have been set above */
			pg_atomic_write_u32(MyMaybeSleepingOnInterrupts, 0);

			break;
		}

		/*
		 * Wait for events using the readiness primitive chosen at the top of
		 * this file. If -1 is returned, a timeout has occurred, if 0 we have
		 * to retry, everything >= 1 is the number of returned events.
		 */
		rc = WaitEventSetWaitBlock(set, cur_timeout,
								   occurred_events, nevents);

		if (set->interrupt_mask != 0)
		{
			Assert(pg_atomic_read_u32(MyMaybeSleepingOnInterrupts) == set->interrupt_mask);
			pg_atomic_write_u32(MyMaybeSleepingOnInterrupts, 0);
		}

		if (rc == -1)
			break;				/* timeout occurred */
		else
			returned_events = rc;

		/* If we're not done, update cur_timeout for next iteration */
		if (returned_events == 0 && timeout >= 0)
		{
			INSTR_TIME_SET_CURRENT(cur_time);
			INSTR_TIME_SUBTRACT(cur_time, start_time);
			cur_timeout = timeout - (long) INSTR_TIME_GET_MILLISEC(cur_time);
			if (cur_timeout <= 0)
				break;
		}
	}
#ifndef WIN32
	waiting = false;
#endif

	pgstat_report_wait_end();

	return returned_events;
}


#if defined(WAIT_USE_EPOLL)

/*
 * Wait using linux's epoll_wait(2).
 *
 * This is the preferable wait method, as several readiness notifications are
 * delivered, without having to iterate through all of set->events. The return
 * epoll_event struct contain a pointer to our events, making association
 * easy.
 */
static inline int
WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
					  WaitEvent *occurred_events, int nevents)
{
	int			returned_events = 0;
	int			rc;
	WaitEvent  *cur_event;
	struct epoll_event *cur_epoll_event;

	/* Sleep */
	rc = epoll_wait(set->epoll_fd, set->epoll_ret_events,
					Min(nevents, set->nevents_space), cur_timeout);

	/* Check return code */
	if (rc < 0)
	{
		/* EINTR is okay, otherwise complain */
		if (errno != EINTR)
		{
			waiting = false;
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("%s() failed: %m",
							"epoll_wait")));
		}
		return 0;
	}
	else if (rc == 0)
	{
		/* timeout exceeded */
		return -1;
	}

	/*
	 * At least one event occurred, iterate over the returned epoll events
	 * until they're either all processed, or we've returned all the events
	 * the caller desired.
	 */
	for (cur_epoll_event = set->epoll_ret_events;
		 cur_epoll_event < (set->epoll_ret_events + rc) &&
		 returned_events < nevents;
		 cur_epoll_event++)
	{
		/* epoll's data pointer is set to the associated WaitEvent */
		cur_event = (WaitEvent *) cur_epoll_event->data.ptr;

		occurred_events->pos = cur_event->pos;
		occurred_events->user_data = cur_event->user_data;
		occurred_events->events = 0;

		if (cur_event->events == WL_INTERRUPT &&
			cur_epoll_event->events & (EPOLLIN | EPOLLERR | EPOLLHUP))
		{
			/* Drain the signalfd / thread wakup fd. */
			drain();

			if (set->interrupt_mask != 0 && (pg_atomic_read_u32(MyPendingInterrupts) & set->interrupt_mask) != 0)
			{
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_INTERRUPT;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events == WL_POSTMASTER_DEATH &&
				 cur_epoll_event->events & (EPOLLIN | EPOLLERR | EPOLLHUP))
		{
			/*
			 * We expect an EPOLLHUP when the remote end is closed, but
			 * because we don't expect the pipe to become readable or to have
			 * any errors either, treat those cases as postmaster death, too.
			 *
			 * Be paranoid about a spurious event signaling the postmaster as
			 * being dead.  There have been reports about that happening with
			 * older primitives (select(2) to be specific), and a spurious
			 * WL_POSTMASTER_DEATH event would be painful. Re-checking doesn't
			 * cost much.
			 */
			if (!PostmasterIsAliveInternal())
			{
				if (set->exit_on_postmaster_death)
					proc_exit(1);
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_POSTMASTER_DEATH;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events & (WL_SOCKET_READABLE |
									  WL_SOCKET_WRITEABLE |
									  WL_SOCKET_CLOSED))
		{
			Assert(cur_event->fd != PGINVALID_SOCKET);

			if ((cur_event->events & WL_SOCKET_READABLE) &&
				(cur_epoll_event->events & (EPOLLIN | EPOLLERR | EPOLLHUP)))
			{
				/* data available in socket, or EOF */
				occurred_events->events |= WL_SOCKET_READABLE;
			}

			if ((cur_event->events & WL_SOCKET_WRITEABLE) &&
				(cur_epoll_event->events & (EPOLLOUT | EPOLLERR | EPOLLHUP)))
			{
				/* writable, or EOF */
				occurred_events->events |= WL_SOCKET_WRITEABLE;
			}

			if ((cur_event->events & WL_SOCKET_CLOSED) &&
				(cur_epoll_event->events & (EPOLLRDHUP | EPOLLERR | EPOLLHUP)))
			{
				/* remote peer shut down, or error */
				occurred_events->events |= WL_SOCKET_CLOSED;
			}

			if (occurred_events->events != 0)
			{
				occurred_events->fd = cur_event->fd;
				occurred_events++;
				returned_events++;
			}
		}
	}

	return returned_events;
}

#elif defined(WAIT_USE_KQUEUE)

/*
 * Wait using kevent(2) on BSD-family systems and macOS.
 *
 * For now this mirrors the epoll code, but in future it could modify the fd
 * set in the same call to kevent as it uses for waiting instead of doing that
 * with separate system calls.
 */
static int
WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
					  WaitEvent *occurred_events, int nevents)
{
	int			returned_events = 0;
	int			rc;
	WaitEvent  *cur_event;
	struct kevent *cur_kqueue_event;
	struct timespec timeout;
	struct timespec *timeout_p;

	if (cur_timeout < 0)
		timeout_p = NULL;
	else
	{
		timeout.tv_sec = cur_timeout / 1000;
		timeout.tv_nsec = (cur_timeout % 1000) * 1000000;
		timeout_p = &timeout;
	}

	/*
	 * Report postmaster events discovered by WaitEventAdjustKqueue() or an
	 * earlier call to WaitEventSetWait().
	 */
	if (unlikely(set->report_postmaster_not_running))
	{
		if (set->exit_on_postmaster_death)
			proc_exit(1);
		occurred_events->fd = PGINVALID_SOCKET;
		occurred_events->events = WL_POSTMASTER_DEATH;
		return 1;
	}

	/* Sleep */
	rc = kevent(set->kqueue_fd, NULL, 0,
				set->kqueue_ret_events,
				Min(nevents, set->nevents_space),
				timeout_p);

	/* Check return code */
	if (rc < 0)
	{
		/* EINTR is okay, otherwise complain */
		if (errno != EINTR)
		{
			waiting = false;
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("%s() failed: %m",
							"kevent")));
		}
		return 0;
	}
	else if (rc == 0)
	{
		/* timeout exceeded */
		return -1;
	}

	/*
	 * At least one event occurred, iterate over the returned kqueue events
	 * until they're either all processed, or we've returned all the events
	 * the caller desired.
	 */
	for (cur_kqueue_event = set->kqueue_ret_events;
		 cur_kqueue_event < (set->kqueue_ret_events + rc) &&
		 returned_events < nevents;
		 cur_kqueue_event++)
	{
		/* kevent's udata points to the associated WaitEvent */
		cur_event = AccessWaitEvent(cur_kqueue_event);

		occurred_events->pos = cur_event->pos;
		occurred_events->user_data = cur_event->user_data;
		occurred_events->events = 0;

		if (cur_event->events == WL_INTERRUPT &&
			cur_kqueue_event->filter == EVFILT_SIGNAL)
		{
			if (set->interrupt_mask != 0 && (pg_atomic_read_u32(MyPendingInterrupts) & set->interrupt_mask) != 0)
			{
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_INTERRUPT;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events == WL_POSTMASTER_DEATH &&
				 cur_kqueue_event->filter == EVFILT_PROC &&
				 (cur_kqueue_event->fflags & NOTE_EXIT) != 0)
		{
			/*
			 * The kernel will tell this kqueue object only once about the
			 * exit of the postmaster, so let's remember that for next time so
			 * that we provide level-triggered semantics.
			 */
			set->report_postmaster_not_running = true;

			if (set->exit_on_postmaster_death)
				proc_exit(1);
			occurred_events->fd = PGINVALID_SOCKET;
			occurred_events->events = WL_POSTMASTER_DEATH;
			occurred_events++;
			returned_events++;
		}
		else if (cur_event->events & (WL_SOCKET_READABLE |
									  WL_SOCKET_WRITEABLE |
									  WL_SOCKET_CLOSED))
		{
			Assert(cur_event->fd >= 0);

			if ((cur_event->events & WL_SOCKET_READABLE) &&
				(cur_kqueue_event->filter == EVFILT_READ))
			{
				/* readable, or EOF */
				occurred_events->events |= WL_SOCKET_READABLE;
			}

			if ((cur_event->events & WL_SOCKET_CLOSED) &&
				(cur_kqueue_event->filter == EVFILT_READ) &&
				(cur_kqueue_event->flags & EV_EOF))
			{
				/* the remote peer has shut down */
				occurred_events->events |= WL_SOCKET_CLOSED;
			}

			if ((cur_event->events & WL_SOCKET_WRITEABLE) &&
				(cur_kqueue_event->filter == EVFILT_WRITE))
			{
				/* writable, or EOF */
				occurred_events->events |= WL_SOCKET_WRITEABLE;
			}

			if (occurred_events->events != 0)
			{
				occurred_events->fd = cur_event->fd;
				occurred_events++;
				returned_events++;
			}
		}
	}

	return returned_events;
}

#elif defined(WAIT_USE_POLL)

/*
 * Wait using poll(2).
 *
 * This allows to receive readiness notifications for several events at once,
 * but requires iterating through all of set->pollfds.
 */
static inline int
WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
					  WaitEvent *occurred_events, int nevents)
{
	int			returned_events = 0;
	int			rc;
	WaitEvent  *cur_event;
	struct pollfd *cur_pollfd;

	/* Sleep */
	rc = poll(set->pollfds, set->nevents, (int) cur_timeout);

	/* Check return code */
	if (rc < 0)
	{
		/* EINTR is okay, otherwise complain */
		if (errno != EINTR)
		{
			waiting = false;
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("%s() failed: %m",
							"poll")));
		}
		return 0;
	}
	else if (rc == 0)
	{
		/* timeout exceeded */
		return -1;
	}

	for (cur_event = set->events, cur_pollfd = set->pollfds;
		 cur_event < (set->events + set->nevents) &&
		 returned_events < nevents;
		 cur_event++, cur_pollfd++)
	{
		/* no activity on this FD, skip */
		if (cur_pollfd->revents == 0)
			continue;

		occurred_events->pos = cur_event->pos;
		occurred_events->user_data = cur_event->user_data;
		occurred_events->events = 0;

		if (cur_event->events == WL_INTERRUPT &&
			(cur_pollfd->revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
		{
			/* There's data in the self-pipe, clear it. */
			drain();

			if (set->interrupt_mask != 0 && (pg_atomic_read_u32(MyPendingInterrupts) & set->interrupt_mask) != 0)
			{
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_INTERRUPT;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events == WL_POSTMASTER_DEATH &&
				 (cur_pollfd->revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
		{
			/*
			 * We expect an POLLHUP when the remote end is closed, but because
			 * we don't expect the pipe to become readable or to have any
			 * errors either, treat those cases as postmaster death, too.
			 *
			 * Be paranoid about a spurious event signaling the postmaster as
			 * being dead.  There have been reports about that happening with
			 * older primitives (select(2) to be specific), and a spurious
			 * WL_POSTMASTER_DEATH event would be painful. Re-checking doesn't
			 * cost much.
			 */
			if (!PostmasterIsAliveInternal())
			{
				if (set->exit_on_postmaster_death)
					proc_exit(1);
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_POSTMASTER_DEATH;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events & (WL_SOCKET_READABLE |
									  WL_SOCKET_WRITEABLE |
									  WL_SOCKET_CLOSED))
		{
			int			errflags = POLLHUP | POLLERR | POLLNVAL;

			Assert(cur_event->fd >= PGINVALID_SOCKET);

			if ((cur_event->events & WL_SOCKET_READABLE) &&
				(cur_pollfd->revents & (POLLIN | errflags)))
			{
				/* data available in socket, or EOF */
				occurred_events->events |= WL_SOCKET_READABLE;
			}

			if ((cur_event->events & WL_SOCKET_WRITEABLE) &&
				(cur_pollfd->revents & (POLLOUT | errflags)))
			{
				/* writeable, or EOF */
				occurred_events->events |= WL_SOCKET_WRITEABLE;
			}

#ifdef POLLRDHUP
			if ((cur_event->events & WL_SOCKET_CLOSED) &&
				(cur_pollfd->revents & (POLLRDHUP | errflags)))
			{
				/* remote peer closed, or error */
				occurred_events->events |= WL_SOCKET_CLOSED;
			}
#endif

			if (occurred_events->events != 0)
			{
				occurred_events->fd = cur_event->fd;
				occurred_events++;
				returned_events++;
			}
		}
	}
	return returned_events;
}

#elif defined(WAIT_USE_WIN32)

/*
 * Wait using Windows' WaitForMultipleObjects().  Each call only "consumes" one
 * event, so we keep calling until we've filled up our output buffer to match
 * the behavior of the other implementations.
 *
 * https://blogs.msdn.microsoft.com/oldnewthing/20150409-00/?p=44273
 */
static inline int
WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
					  WaitEvent *occurred_events, int nevents)
{
	int			returned_events = 0;
	DWORD		rc;
	WaitEvent  *cur_event;

	/* Reset any wait events that need it */
	for (cur_event = set->events;
		 cur_event < (set->events + set->nevents);
		 cur_event++)
	{
		if (cur_event->reset)
		{
			WaitEventAdjustWin32(set, cur_event);
			cur_event->reset = false;
		}

		/*
		 * We need to use different event object depending on whether "local"
		 * or "shared memory" interrupts are in use. There's no easy way to
		 * adjust all existing WaitEventSet when you switch from local to
		 * shared or back, so we refresh it on every call.
		 */
		if (cur_event->events & WL_INTERRUPT)
			WaitEventAdjustWin32(set, cur_event);

		/*
		 * We associate the socket with a new event handle for each
		 * WaitEventSet.  FD_CLOSE is only generated once if the other end
		 * closes gracefully.  Therefore we might miss the FD_CLOSE
		 * notification, if it was delivered to another event after we stopped
		 * waiting for it.  Close that race by peeking for EOF after setting
		 * up this handle to receive notifications, and before entering the
		 * sleep.
		 *
		 * XXX If we had one event handle for the lifetime of a socket, we
		 * wouldn't need this.
		 */
		if (cur_event->events & WL_SOCKET_READABLE)
		{
			char		c;
			WSABUF		buf;
			DWORD		received;
			DWORD		flags;

			buf.buf = &c;
			buf.len = 1;
			flags = MSG_PEEK;
			if (WSARecv(cur_event->fd, &buf, 1, &received, &flags, NULL, NULL) == 0)
			{
				occurred_events->pos = cur_event->pos;
				occurred_events->user_data = cur_event->user_data;
				occurred_events->events = WL_SOCKET_READABLE;
				occurred_events->fd = cur_event->fd;
				return 1;
			}
		}

		/*
		 * Windows does not guarantee to log an FD_WRITE network event
		 * indicating that more data can be sent unless the previous send()
		 * failed with WSAEWOULDBLOCK.  While our caller might well have made
		 * such a call, we cannot assume that here.  Therefore, if waiting for
		 * write-ready, force the issue by doing a dummy send().  If the dummy
		 * send() succeeds, assume that the socket is in fact write-ready, and
		 * return immediately.  Also, if it fails with something other than
		 * WSAEWOULDBLOCK, return a write-ready indication to let our caller
		 * deal with the error condition.
		 */
		if (cur_event->events & WL_SOCKET_WRITEABLE)
		{
			char		c;
			WSABUF		buf;
			DWORD		sent;
			int			r;

			buf.buf = &c;
			buf.len = 0;

			r = WSASend(cur_event->fd, &buf, 1, &sent, 0, NULL, NULL);
			if (r == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
			{
				occurred_events->pos = cur_event->pos;
				occurred_events->user_data = cur_event->user_data;
				occurred_events->events = WL_SOCKET_WRITEABLE;
				occurred_events->fd = cur_event->fd;
				return 1;
			}
		}
	}

	/*
	 * Sleep.
	 *
	 * Need to wait for ->nevents + 1, because signal handle is in [0].
	 */
	rc = WaitForMultipleObjects(set->nevents + 1, set->handles, FALSE,
								cur_timeout);

	/* Check return code */
	if (rc == WAIT_FAILED)
		elog(ERROR, "WaitForMultipleObjects() failed: error code %lu",
			 GetLastError());
	else if (rc == WAIT_TIMEOUT)
	{
		/* timeout exceeded */
		return -1;
	}

	if (rc == WAIT_OBJECT_0)
	{
		/* Service newly-arrived signals */
		pgwin32_dispatch_queued_signals();
		return 0;				/* retry */
	}

	/*
	 * With an offset of one, due to the always present pgwin32_signal_event,
	 * the handle offset directly corresponds to a wait event.
	 */
	cur_event = (WaitEvent *) &set->events[rc - WAIT_OBJECT_0 - 1];

	for (;;)
	{
		int			next_pos;
		int			count;

		occurred_events->pos = cur_event->pos;
		occurred_events->user_data = cur_event->user_data;
		occurred_events->events = 0;

		if (cur_event->events == WL_INTERRUPT)
		{
			if (!ResetEvent(set->handles[cur_event->pos + 1]))
				elog(ERROR, "ResetEvent failed: error code %lu", GetLastError());

			if (set->interrupt_mask != 0 && (pg_atomic_read_u32(MyPendingInterrupts) & set->interrupt_mask) != 0)
			{
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_INTERRUPT;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events == WL_POSTMASTER_DEATH)
		{
			/*
			 * Postmaster apparently died.  Since the consequences of falsely
			 * returning WL_POSTMASTER_DEATH could be pretty unpleasant, we
			 * take the trouble to positively verify this with
			 * PostmasterIsAlive(), even though there is no known reason to
			 * think that the event could be falsely set on Windows.
			 */
			if (!PostmasterIsAliveInternal())
			{
				if (set->exit_on_postmaster_death)
					proc_exit(1);
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_POSTMASTER_DEATH;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events & WL_SOCKET_MASK)
		{
			WSANETWORKEVENTS resEvents;
			HANDLE		handle = set->handles[cur_event->pos + 1];

			Assert(cur_event->fd);

			occurred_events->fd = cur_event->fd;

			ZeroMemory(&resEvents, sizeof(resEvents));
			if (WSAEnumNetworkEvents(cur_event->fd, handle, &resEvents) != 0)
				elog(ERROR, "failed to enumerate network events: error code %d",
					 WSAGetLastError());
			if ((cur_event->events & WL_SOCKET_READABLE) &&
				(resEvents.lNetworkEvents & FD_READ))
			{
				/* data available in socket */
				occurred_events->events |= WL_SOCKET_READABLE;

				/*------
				 * WaitForMultipleObjects doesn't guarantee that a read event
				 * will be returned if the interrupt is set at the same time.  Even
				 * if it did, the caller might drop that event expecting it to
				 * reoccur on next call.  So, we must force the event to be
				 * reset if this WaitEventSet is used again in order to avoid
				 * an indefinite hang.
				 *
				 * Refer
				 * https://msdn.microsoft.com/en-us/library/windows/desktop/ms741576(v=vs.85).aspx
				 * for the behavior of socket events.
				 *------
				 */
				cur_event->reset = true;
			}
			if ((cur_event->events & WL_SOCKET_WRITEABLE) &&
				(resEvents.lNetworkEvents & FD_WRITE))
			{
				/* writeable */
				occurred_events->events |= WL_SOCKET_WRITEABLE;
			}
			if ((cur_event->events & WL_SOCKET_CONNECTED) &&
				(resEvents.lNetworkEvents & FD_CONNECT))
			{
				/* connected */
				occurred_events->events |= WL_SOCKET_CONNECTED;
			}
			if ((cur_event->events & WL_SOCKET_ACCEPT) &&
				(resEvents.lNetworkEvents & FD_ACCEPT))
			{
				/* incoming connection could be accepted */
				occurred_events->events |= WL_SOCKET_ACCEPT;
			}
			if (resEvents.lNetworkEvents & FD_CLOSE)
			{
				/* EOF/error, so signal all caller-requested socket flags */
				occurred_events->events |= (cur_event->events & WL_SOCKET_MASK);
			}

			if (occurred_events->events != 0)
			{
				occurred_events++;
				returned_events++;
			}
		}

		/* Is the output buffer full? */
		if (returned_events == nevents)
			break;

		/* Have we run out of possible events? */
		next_pos = cur_event->pos + 1;
		if (next_pos == set->nevents)
			break;

		/*
		 * Poll the rest of the event handles in the array starting at
		 * next_pos being careful to skip over the initial signal handle too.
		 * This time we use a zero timeout.
		 */
		count = set->nevents - next_pos;
		rc = WaitForMultipleObjects(count,
									set->handles + 1 + next_pos,
									false,
									0);

		/*
		 * We don't distinguish between errors and WAIT_TIMEOUT here because
		 * we already have events to report.
		 */
		if (rc < WAIT_OBJECT_0 || rc >= WAIT_OBJECT_0 + count)
			break;

		/* We have another event to decode. */
		cur_event = &set->events[next_pos + (rc - WAIT_OBJECT_0)];
	}

	return returned_events;
}
#endif

/*
 * Return whether the current build options can report WL_SOCKET_CLOSED.
 */
bool
WaitEventSetCanReportClosed(void)
{
#if (defined(WAIT_USE_POLL) && defined(POLLRDHUP)) || \
	defined(WAIT_USE_EPOLL) || \
	defined(WAIT_USE_KQUEUE)
	return true;
#else
	return false;
#endif
}

/*
 * Get the number of wait events registered in a given WaitEventSet.
 */
int
GetNumRegisteredWaitEvents(WaitEventSet *set)
{
	return set->nevents;
}

#if defined(WAIT_USE_SELF_PIPE)

/*
 * WakeupOtherProc and WakupMyProc use SIGURG to wake up the process waiting
 * for an interrupt
 */
static void
interrupt_sigurg_handler(SIGNAL_ARGS)
{
	if (waiting)
		sendSelfPipeByte();
}

/* Send one byte to the self-pipe, to wake up WaitInterrupt */
static void
sendSelfPipeByte(void)
{
	sendPipeWakeupByte(selfpipe_writefd);
}

#endif

/* Send one byte to the self-pipe or a thread wakeup pipe, to wake up WaitLatch */
static void
sendPipeWakeupByte(int writefd)
{
	int			rc;
	char		dummy = 0;

retry:
	rc = write(writefd, &dummy, 1);
	if (rc < 0)
	{
		/* If interrupted by signal, just retry */
		if (errno == EINTR)
			goto retry;

		/*
		 * If the pipe is full, we don't need to retry, the data that's there
		 * already is enough to wake up WaitInterrupt.
		 */
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;

		/*
		 * Oops, the write() failed for some other reason. We might be in a
		 * signal handler, so it's not safe to elog(). We have no choice but
		 * silently ignore the error.
		 */
		return;
	}
}

/*
 * Read all available data from self-pipe or signalfd.
 *
 * Note: this is only called when waiting = true.  If it fails and doesn't
 * return, it must reset that flag first (though ideally, this will never
 * happen).
 */
static void
drain(void)
{
	char		buf[1024];
	int			rc;
	int			fd;

	if (IsMultiThreaded)
		fd = thread_wakeup_readfds[IsUnderPostmaster ? MyPMChildSlot : 0];
	else
	{
#ifdef WAIT_USE_SELF_PIPE
		fd = selfpipe_readfd;
#elif defined(WAIT_USE_SIGNALFD)
		fd = signal_fd;
#else
		elog(PANIC, "drain() called unexpectedly");
#endif
	}
	for (;;)
	{
		rc = read(fd, buf, sizeof(buf));
		if (rc < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;			/* the descriptor is empty */
			else if (errno == EINTR)
				continue;		/* retry */
			else
			{
				waiting = false;
#ifdef WAIT_USE_SELF_PIPE
				elog(ERROR, "read() on self-pipe failed: %m");
#else
				elog(ERROR, "read() on signalfd failed: %m");
#endif
			}
		}
		else if (rc == 0)
		{
			waiting = false;
#ifdef WAIT_USE_SELF_PIPE
			elog(ERROR, "unexpected EOF on self-pipe");
#else
			elog(ERROR, "unexpected EOF on signalfd");
#endif
		}
		else if (rc < sizeof(buf))
		{
			/* we successfully drained the pipe; no need to read() again */
			break;
		}
		/* else buffer wasn't big enough, so read again */
	}
}

static void
ResOwnerReleaseWaitEventSet(Datum res)
{
	WaitEventSet *set = (WaitEventSet *) DatumGetPointer(res);

	Assert(set->owner != NULL);
	set->owner = NULL;
	FreeWaitEventSet(set);
}