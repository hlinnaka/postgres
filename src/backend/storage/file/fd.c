/*-------------------------------------------------------------------------
 *
 * fd.c
 *	  Virtual file descriptor code.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/fd.c
 *
 * NOTES:
 *
 * This code manages a cache of 'virtual' file descriptors (VFDs).
 * The server opens many file descriptors for a variety of reasons,
 * including base tables, scratch files (e.g., sort and hash spool
 * files), and random calls to C library routines like system(3); it
 * is quite easy to exceed system limits on the number of open files a
 * single process can have.  (This is around 1024 on many modern
 * operating systems, but may be lower on others.)
 *
 * VFDs are managed as an LRU pool, with actual OS file descriptors
 * being opened and closed as needed.  Obviously, if a routine is
 * opened using these interfaces, all subsequent operations must also
 * be through these interfaces (the File type is not a real file
 * descriptor).
 *
 * For this scheme to work, most (if not all) routines throughout the
 * server should use these interfaces instead of calling the C library
 * routines (e.g., open(2) and fopen(3)) themselves.  Otherwise, we
 * may find ourselves short of real file descriptors anyway.
 *
 * INTERFACE ROUTINES
 *
 * PathNameOpenFile and OpenTemporaryFile are used to open virtual files.
 * A File opened with OpenTemporaryFile is automatically deleted when the
 * File is closed, either explicitly or implicitly at end of transaction or
 * process exit. PathNameOpenFile is intended for files that are held open
 * for a long time, like relation files. It is the caller's responsibility
 * to close them, there is no automatic mechanism in fd.c for that.
 *
 * PathName(Create|Open|Delete)Temporary(File|Dir) are used to manage
 * temporary files that have names so that they can be shared between
 * backends.  Such files are automatically closed and count against the
 * temporary file limit of the backend that creates them, but unlike anonymous
 * files they are not automatically deleted.  See sharedfileset.c for a shared
 * ownership mechanism that provides automatic cleanup for shared files when
 * the last of a group of backends detaches.
 *
 * AllocateFile, AllocateDir, OpenPipeStream and OpenTransientFile are
 * wrappers around fopen(3), opendir(3), popen(3) and open(2), respectively.
 * They behave like the corresponding native functions, except that the handle
 * is registered with the current subtransaction, and will be automatically
 * closed at abort. These are intended mainly for short operations like
 * reading a configuration file; there is a limit on the number of files that
 * can be opened using these functions at any one time.
 *
 * Finally, BasicOpenFile is just a thin wrapper around open() that can
 * release file descriptors in use by the virtual file descriptors if
 * necessary. There is no automatic cleanup of file descriptors returned by
 * BasicOpenFile, it is solely the caller's responsibility to close the file
 * descriptor by calling close(2).
 *
 * If a non-virtual file descriptor needs to be held open for any length of
 * time, report it to fd.c by calling AcquireExternalFD or ReserveExternalFD
 * (and eventually ReleaseExternalFD), so that we can take it into account
 * while deciding how many VFDs can be open.  This applies to FDs obtained
 * with BasicOpenFile as well as those obtained without use of any fd.c API.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <dirent.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/resource.h>		/* for getrlimit */
#include <sys/stat.h>
#include <sys/types.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

#include "access/xact.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/aio.h"
#include "storage/datadir.h"
#include "storage/fd.h"
#include "storage/file_utils.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/resowner.h"
#include "utils/varlena.h"
#include "utils/wait_event.h"

/*
 * We must leave some file descriptors free for system(), the dynamic loader,
 * and other code that tries to open files without consulting fd.c.  This
 * is the number left free.  (While we try fairly hard to prevent EMFILE
 * errors, there's never any guarantee that we won't get ENFILE due to
 * other processes chewing up FDs.  So it's a bad idea to try to open files
 * without consulting fd.c.  Nonetheless we cannot control all code.)
 *
 * Because this is just a fixed setting, we are effectively assuming that
 * no such code will leave FDs open over the long term; otherwise the slop
 * is likely to be insufficient.  Note in particular that we expect that
 * loading a shared library does not result in any permanent increase in
 * the number of open files.  (This appears to be true on most if not
 * all platforms as of Feb 2004.)
 */
#define NUM_RESERVED_FDS		10

/*
 * If we have fewer than this many usable FDs after allowing for the reserved
 * ones, choke.  (This value is chosen to work with "ulimit -n 64", but not
 * much less than that.  Note that this value ensures numExternalFDs can be
 * at least 16; as of this writing, the contrib/postgres_fdw regression tests
 * will not pass unless that can grow to at least 14.)
 */
#define FD_MINFREE				48

/*
 * A number of platforms allow individual processes to open many more files
 * than they can really support when *many* processes do the same thing.
 * This GUC parameter lets the DBA limit max_safe_fds to something less than
 * what the postmaster's initial probe suggests will work.
 */
int			max_files_per_process = 1000;

/*
 * Maximum number of file descriptors to open for operations that fd.c knows
 * about (VFDs, AllocateFile etc, or "external" FDs).  This is initialized
 * to a conservative value, and remains that way indefinitely in bootstrap or
 * standalone-backend cases.  In normal postmaster operation, the postmaster
 * calls set_max_safe_fds() late in initialization to update the value, and
 * that value is then inherited by forked subprocesses.
 *
 * Note: the value of max_files_per_process is taken into account while
 * setting this variable, and so need not be tested separately.
 */
int			max_safe_fds = FD_MINFREE;	/* default if not changed */

/* How data files should be bulk-extended with zeros. */
int			file_extend_method = DEFAULT_FILE_EXTEND_METHOD;

/* Which kinds of files should be opened with PG_O_DIRECT. */
int			io_direct_flags;

/* Debugging.... */

#ifdef FDDEBUG
#define DO_DB(A) \
	do { \
		int			_do_db_save_errno = errno; \
		A; \
		errno = _do_db_save_errno; \
	} while (0)
#else
#define DO_DB(A) \
	((void) 0)
#endif

#define VFD_CLOSED (-1)

#define FileIsValid(file) \
	((file) > 0 && (file) < (int) SizeVfdCache && VfdCache[file].fileName != NULL)

#define FileIsNotOpen(file) (VfdCache[file].fd == VFD_CLOSED)

typedef struct vfd
{
	int			fd;				/* current FD, or VFD_CLOSED if none */
	unsigned short tempFlags;	/* bitflags for temporary file VFDs */
	ResourceOwner resowner;		/* owner, for automatic cleanup */
	File		nextFree;		/* link to next free VFD, if in freelist */
	File		lruMoreRecently;	/* doubly linked recency-of-use list */
	File		lruLessRecently;
	pgoff_t		fileSize;		/* current size of file (0 if not temporary) */
	char	   *fileName;		/* name of file, or NULL for unused VFD */
	/* NB: fileName is malloc'd, and must be free'd when closing the VFD */
	int			fileFlags;		/* open(2) flags for (re)opening the file */
	mode_t		fileMode;		/* mode to pass to open(2) */
} Vfd;

/*
 * Virtual File Descriptor array pointer and size.  This grows as
 * needed.  'File' values are indexes into this array.
 * Note that VfdCache[0] is not a usable VFD, just a list header.
 */
static Vfd *VfdCache;
static Size SizeVfdCache = 0;

/*
 * Number of file descriptors known to be in use by VFD entries.
 */
static int	nfile = 0;

/*
 * Tracks the total size of all temporary files.  Note: when temp_file_limit
 * is being enforced, this cannot overflow since the limit cannot be more
 * than INT_MAX kilobytes.  When not enforcing, it could theoretically
 * overflow, but we don't care.
 */
static uint64 temporary_files_size = 0;

/*
 * List of OS handles opened with AllocateFile, AllocateDir and
 * OpenTransientFile.
 */
typedef enum
{
	AllocateDescFile,
	AllocateDescPipe,
	AllocateDescDir,
	AllocateDescRawFD,
} AllocateDescKind;

typedef struct
{
	AllocateDescKind kind;
	SubTransactionId create_subid;
	union
	{
		FILE	   *file;
		DIR		   *dir;
		int			fd;
	}			desc;
} AllocateDesc;

static int	numAllocatedDescs = 0;
static int	maxAllocatedDescs = 0;
static AllocateDesc *allocatedDescs = NULL;

/*
 * Number of open "external" FDs reported to Reserve/ReleaseExternalFD.
 */
static int	numExternalFDs = 0;


/*--------------------
 *
 * Private Routines
 *
 * Delete		   - delete a file from the Lru ring
 * LruDelete	   - remove a file from the Lru ring and close its FD
 * Insert		   - put a file at the front of the Lru ring
 * LruInsert	   - put a file at the front of the Lru ring and open it
 * ReleaseLruFile  - Release an fd by closing the last entry in the Lru ring
 * ReleaseLruFiles - Release fd(s) until we're under the max_safe_fds limit
 * AllocateVfd	   - grab a free (or new) file record (from VfdCache)
 * FreeVfd		   - free a file record
 *
 * The Least Recently Used ring is a doubly linked list that begins and
 * ends on element zero.  Element zero is special -- it doesn't represent
 * a file and its "fd" field always == VFD_CLOSED.  Element zero is just an
 * anchor that shows us the beginning/end of the ring.
 * Only VFD elements that are currently really open (have an FD assigned) are
 * in the Lru ring.  Elements that are "virtually" open can be recognized
 * by having a non-null fileName field.
 *
 * example:
 *
 *	   /--less----\				   /---------\
 *	   v		   \			  v			  \
 *	 #0 --more---> LeastRecentlyUsed --more-\ \
 *	  ^\									| |
 *	   \\less--> MostRecentlyUsedFile	<---/ |
 *		\more---/					 \--less--/
 *
 *--------------------
 */
static void Delete(File file);
static void LruDelete(File file);
static void Insert(File file);
static int	LruInsert(File file);
static bool ReleaseLruFile(void);
static void ReleaseLruFiles(void);
static File AllocateVfd(void);
static void FreeVfd(File file);

static int	FileAccess(File file);
static bool reserveAllocatedDesc(void);
static int	FreeDesc(AllocateDesc *desc);

/* ResourceOwner callbacks to hold virtual file descriptors */
static void ResOwnerReleaseFile(Datum res);
static char *ResOwnerPrintFile(Datum res);

static const ResourceOwnerDesc file_resowner_desc =
{
	.name = "File",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_FILES,
	.ReleaseResource = ResOwnerReleaseFile,
	.DebugPrint = ResOwnerPrintFile
};

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void
ResourceOwnerRememberFile(ResourceOwner owner, File file)
{
	ResourceOwnerRemember(owner, Int32GetDatum(file), &file_resowner_desc);
}
static inline void
ResourceOwnerForgetFile(ResourceOwner owner, File file)
{
	ResourceOwnerForget(owner, Int32GetDatum(file), &file_resowner_desc);
}

/*
 * InitFileAccess --- initialize this module during backend startup
 *
 * This is called during either normal or standalone backend start.
 * It is *not* called in the postmaster.
 *
 * Note that this does not initialize temporary file access, that is
 * separately initialized via InitTemporaryFileAccess().
 */
void
InitFileAccess(void)
{
	Assert(SizeVfdCache == 0);	/* call me only once */

	/* initialize cache header entry */
	VfdCache = (Vfd *) malloc(sizeof(Vfd));
	if (VfdCache == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	MemSet(&(VfdCache[0]), 0, sizeof(Vfd));
	VfdCache->fd = VFD_CLOSED;

	SizeVfdCache = 1;
}

/*
 * count_usable_fds --- count how many FDs the system will let us open,
 *		and estimate how many are already open.
 *
 * We stop counting if usable_fds reaches max_to_probe.  Note: a small
 * value of max_to_probe might result in an underestimate of already_open;
 * we must fill in any "gaps" in the set of used FDs before the calculation
 * of already_open will give the right answer.  In practice, max_to_probe
 * of a couple of dozen should be enough to ensure good results.
 *
 * We assume stderr (FD 2) is available for dup'ing.  While the calling
 * script could theoretically close that, it would be a really bad idea,
 * since then one risks loss of error messages from, e.g., libc.
 */
static void
count_usable_fds(int max_to_probe, int *usable_fds, int *already_open)
{
	int		   *fd;
	int			size;
	int			used = 0;
	int			highestfd = 0;
	int			j;

#ifdef HAVE_GETRLIMIT
	struct rlimit rlim;
	int			getrlimit_status;
#endif

	size = 1024;
	fd = (int *) palloc(size * sizeof(int));

#ifdef HAVE_GETRLIMIT
	getrlimit_status = getrlimit(RLIMIT_NOFILE, &rlim);
	if (getrlimit_status != 0)
		ereport(WARNING, (errmsg("getrlimit failed: %m")));
#endif							/* HAVE_GETRLIMIT */

	/* dup until failure or probe limit reached */
	for (;;)
	{
		int			thisfd;

#ifdef HAVE_GETRLIMIT

		/*
		 * don't go beyond RLIMIT_NOFILE; causes irritating kernel logs on
		 * some platforms
		 */
		if (getrlimit_status == 0 && highestfd >= rlim.rlim_cur - 1)
			break;
#endif

		thisfd = dup(2);
		if (thisfd < 0)
		{
			/* Expect EMFILE or ENFILE, else it's fishy */
			if (errno != EMFILE && errno != ENFILE)
				elog(WARNING, "duplicating stderr file descriptor failed after %d successes: %m", used);
			break;
		}

		if (used >= size)
		{
			size *= 2;
			fd = (int *) repalloc(fd, size * sizeof(int));
		}
		fd[used++] = thisfd;

		if (highestfd < thisfd)
			highestfd = thisfd;

		if (used >= max_to_probe)
			break;
	}

	/* release the files we opened */
	for (j = 0; j < used; j++)
		close(fd[j]);

	pfree(fd);

	/*
	 * Return results.  usable_fds is just the number of successful dups. We
	 * assume that the system limit is highestfd+1 (remember 0 is a legal FD
	 * number) and so already_open is highestfd+1 - usable_fds.
	 */
	*usable_fds = used;
	*already_open = highestfd + 1 - used;
}

/*
 * set_max_safe_fds
 *		Determine number of file descriptors that fd.c is allowed to use
 */
void
set_max_safe_fds(void)
{
	int			usable_fds;
	int			already_open;

	/*----------
	 * We want to set max_safe_fds to
	 *			MIN(usable_fds, max_files_per_process)
	 * less the slop factor for files that are opened without consulting
	 * fd.c.  This ensures that we won't allow to open more than
	 * max_files_per_process, or the experimentally-determined EMFILE limit,
	 * additional files.
	 *----------
	 */
	count_usable_fds(max_files_per_process,
					 &usable_fds, &already_open);

	max_safe_fds = Min(usable_fds, max_files_per_process);

	/*
	 * Take off the FDs reserved for system() etc.
	 */
	max_safe_fds -= NUM_RESERVED_FDS;

	/*
	 * Make sure we still have enough to get by.
	 */
	if (max_safe_fds < FD_MINFREE)
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("insufficient file descriptors available to start server process"),
				 errdetail("System allows %d, server needs at least %d, %d files are already open.",
						   max_safe_fds + NUM_RESERVED_FDS,
						   FD_MINFREE + NUM_RESERVED_FDS,
						   already_open)));

	elog(DEBUG2, "max_safe_fds = %d, usable_fds = %d, already_open = %d",
		 max_safe_fds, usable_fds, already_open);
}

/*
 * Open a file with BasicOpenFilePerm() and pass default file mode for the
 * fileMode parameter.
 */
int
BasicOpenFile(const char *fileName, int fileFlags)
{
	return BasicOpenFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * BasicOpenFilePerm --- same as open(2) except can free other FDs if needed
 *
 * This is exported for use by places that really want a plain kernel FD,
 * but need to be proof against running out of FDs.  Once an FD has been
 * successfully returned, it is the caller's responsibility to ensure that
 * it will not be leaked on ereport()!	Most users should *not* call this
 * routine directly, but instead use the VFD abstraction level, which
 * provides protection against descriptor leaks as well as management of
 * files that need to be open for more than a short period of time.
 *
 * Ideally this should be the *only* direct call of open() in the backend.
 * In practice, the postmaster calls open() directly, and there are some
 * direct open() calls done early in backend startup.  Those are OK since
 * this module wouldn't have any open files to close at that point anyway.
 */
int
BasicOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	int			fd;

tryAgain:
#ifdef PG_O_DIRECT_USE_F_NOCACHE
	fd = open(fileName, fileFlags & ~PG_O_DIRECT, fileMode);
#else
	fd = open(fileName, fileFlags, fileMode);
#endif

	if (fd >= 0)
	{
#ifdef PG_O_DIRECT_USE_F_NOCACHE
		if (fileFlags & PG_O_DIRECT)
		{
			if (fcntl(fd, F_NOCACHE, 1) < 0)
			{
				int			save_errno = errno;

				close(fd);
				errno = save_errno;
				return -1;
			}
		}
#endif

		return fd;				/* success! */
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto tryAgain;
		errno = save_errno;
	}

	return -1;					/* failure */
}

/*
 * AcquireExternalFD - attempt to reserve an external file descriptor
 *
 * This should be used by callers that need to hold a file descriptor open
 * over more than a short interval, but cannot use any of the other facilities
 * provided by this module.
 *
 * The difference between this and the underlying ReserveExternalFD function
 * is that this will report failure (by setting errno and returning false)
 * if "too many" external FDs are already reserved.  This should be used in
 * any code where the total number of FDs to be reserved is not predictable
 * and small.
 */
bool
AcquireExternalFD(void)
{
	/*
	 * We don't want more than max_safe_fds / 3 FDs to be consumed for
	 * "external" FDs.
	 */
	if (numExternalFDs < max_safe_fds / 3)
	{
		ReserveExternalFD();
		return true;
	}
	errno = EMFILE;
	return false;
}

/*
 * ReserveExternalFD - report external consumption of a file descriptor
 *
 * This should be used by callers that need to hold a file descriptor open
 * over more than a short interval, but cannot use any of the other facilities
 * provided by this module.  This just tracks the use of the FD and closes
 * VFDs if needed to ensure we keep NUM_RESERVED_FDS FDs available.
 *
 * Call this directly only in code where failure to reserve the FD would be
 * fatal; for example, the WAL-writing code does so, since the alternative is
 * session failure.  Also, it's very unwise to do so in code that could
 * consume more than one FD per process.
 *
 * Note: as long as everybody plays nice so that NUM_RESERVED_FDS FDs remain
 * available, it doesn't matter too much whether this is called before or
 * after actually opening the FD; but doing so beforehand reduces the risk of
 * an EMFILE failure if not everybody played nice.  In any case, it's solely
 * caller's responsibility to keep the external-FD count in sync with reality.
 */
void
ReserveExternalFD(void)
{
	/*
	 * Release VFDs if needed to stay safe.  Because we do this before
	 * incrementing numExternalFDs, the final state will be as desired, i.e.,
	 * nfile + numAllocatedDescs + numExternalFDs <= max_safe_fds.
	 */
	ReleaseLruFiles();

	numExternalFDs++;
}

/*
 * ReleaseExternalFD - report release of an external file descriptor
 *
 * This is guaranteed not to change errno, so it can be used in failure paths.
 */
void
ReleaseExternalFD(void)
{
	Assert(numExternalFDs > 0);
	numExternalFDs--;
}


#if defined(FDDEBUG)

static void
_dump_lru(void)
{
	int			mru = VfdCache[0].lruLessRecently;
	Vfd		   *vfdP = &VfdCache[mru];
	char		buf[2048];

	snprintf(buf, sizeof(buf), "LRU: MOST %d ", mru);
	while (mru != 0)
	{
		mru = vfdP->lruLessRecently;
		vfdP = &VfdCache[mru];
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d ", mru);
	}
	snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "LEAST");
	elog(LOG, "%s", buf);
}
#endif							/* FDDEBUG */

static void
Delete(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "Delete %d (%s)",
			   file, VfdCache[file].fileName));
	DO_DB(_dump_lru());

	vfdP = &VfdCache[file];

	VfdCache[vfdP->lruLessRecently].lruMoreRecently = vfdP->lruMoreRecently;
	VfdCache[vfdP->lruMoreRecently].lruLessRecently = vfdP->lruLessRecently;

	DO_DB(_dump_lru());
}

static void
LruDelete(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "LruDelete %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	pgaio_closing_fd(vfdP->fd);

	/*
	 * Close the file.  We aren't expecting this to fail; if it does, better
	 * to leak the FD than to mess up our internal state.
	 */
	if (close(vfdP->fd) != 0)
		elog(vfdP->tempFlags & FD_TEMP_FILE_LIMIT ? LOG : data_sync_elevel(LOG),
			 "could not close file \"%s\": %m", vfdP->fileName);
	vfdP->fd = VFD_CLOSED;
	--nfile;

	/* delete the vfd record from the LRU ring */
	Delete(file);
}

static void
Insert(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "Insert %d (%s)",
			   file, VfdCache[file].fileName));
	DO_DB(_dump_lru());

	vfdP = &VfdCache[file];

	vfdP->lruMoreRecently = 0;
	vfdP->lruLessRecently = VfdCache[0].lruLessRecently;
	VfdCache[0].lruLessRecently = file;
	VfdCache[vfdP->lruLessRecently].lruMoreRecently = file;

	DO_DB(_dump_lru());
}

/* returns 0 on success, -1 on re-open failure (with errno set) */
static int
LruInsert(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "LruInsert %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	if (FileIsNotOpen(file))
	{
		/* Close excess kernel FDs. */
		ReleaseLruFiles();

		/*
		 * The open could still fail for lack of file descriptors, eg due to
		 * overall system file table being full.  So, be prepared to release
		 * another FD if necessary...
		 */
		vfdP->fd = BasicOpenFilePerm(vfdP->fileName, vfdP->fileFlags,
									 vfdP->fileMode);
		if (vfdP->fd < 0)
		{
			DO_DB(elog(LOG, "re-open failed: %m"));
			return -1;
		}
		else
		{
			++nfile;
		}
	}

	/*
	 * put it at the head of the Lru ring
	 */

	Insert(file);

	return 0;
}

/*
 * Release one kernel FD by closing the least-recently-used VFD.
 */
static bool
ReleaseLruFile(void)
{
	DO_DB(elog(LOG, "ReleaseLruFile. Opened %d", nfile));

	if (nfile > 0)
	{
		/*
		 * There are opened files and so there should be at least one used vfd
		 * in the ring.
		 */
		Assert(VfdCache[0].lruMoreRecently != 0);
		LruDelete(VfdCache[0].lruMoreRecently);
		return true;			/* freed a file */
	}
	return false;				/* no files available to free */
}

/*
 * Release kernel FDs as needed to get under the max_safe_fds limit.
 * After calling this, it's OK to try to open another file.
 */
static void
ReleaseLruFiles(void)
{
	while (nfile + numAllocatedDescs + numExternalFDs >= max_safe_fds)
	{
		if (!ReleaseLruFile())
			break;
	}
}

static File
AllocateVfd(void)
{
	Index		i;
	File		file;

	DO_DB(elog(LOG, "AllocateVfd. Size %zu", SizeVfdCache));

	Assert(SizeVfdCache > 0);	/* InitFileAccess not called? */

	if (VfdCache[0].nextFree == 0)
	{
		/*
		 * The free list is empty so it is time to increase the size of the
		 * array.  We choose to double it each time this happens. However,
		 * there's not much point in starting *real* small.
		 */
		Size		newCacheSize = SizeVfdCache * 2;
		Vfd		   *newVfdCache;

		if (newCacheSize < 32)
			newCacheSize = 32;

		/*
		 * Be careful not to clobber VfdCache ptr if realloc fails.
		 */
		newVfdCache = (Vfd *) realloc(VfdCache, sizeof(Vfd) * newCacheSize);
		if (newVfdCache == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		VfdCache = newVfdCache;

		/*
		 * Initialize the new entries and link them into the free list.
		 */
		for (i = SizeVfdCache; i < newCacheSize; i++)
		{
			MemSet(&(VfdCache[i]), 0, sizeof(Vfd));
			VfdCache[i].nextFree = i + 1;
			VfdCache[i].fd = VFD_CLOSED;
		}
		VfdCache[newCacheSize - 1].nextFree = 0;
		VfdCache[0].nextFree = SizeVfdCache;

		/*
		 * Record the new size
		 */
		SizeVfdCache = newCacheSize;
	}

	file = VfdCache[0].nextFree;

	VfdCache[0].nextFree = VfdCache[file].nextFree;

	return file;
}

static void
FreeVfd(File file)
{
	Vfd		   *vfdP = &VfdCache[file];

	DO_DB(elog(LOG, "FreeVfd: %d (%s)",
			   file, vfdP->fileName ? vfdP->fileName : ""));

	if (vfdP->fileName != NULL)
	{
		free(vfdP->fileName);
		vfdP->fileName = NULL;
	}
	vfdP->tempFlags = 0x0;

	vfdP->nextFree = VfdCache[0].nextFree;
	VfdCache[0].nextFree = file;
}

/* returns 0 on success, -1 on re-open failure (with errno set) */
static int
FileAccess(File file)
{
	int			returnValue;

	DO_DB(elog(LOG, "FileAccess %d (%s)",
			   file, VfdCache[file].fileName));

	/*
	 * Is the file open?  If not, open it and put it at the head of the LRU
	 * ring (possibly closing the least recently used file to get an FD).
	 */

	if (FileIsNotOpen(file))
	{
		returnValue = LruInsert(file);
		if (returnValue != 0)
			return returnValue;
	}
	else if (VfdCache[0].lruLessRecently != file)
	{
		/*
		 * We now know that the file is open and that it is not the last one
		 * accessed, so we need to move it to the head of the Lru ring.
		 */

		Delete(file);
		Insert(file);
	}

	return 0;
}

/*
 * Called whenever a temporary file is deleted to report its size.
 */
static void
ReportTemporaryFileUsage(const char *path, pgoff_t size)
{
	pgstat_report_tempfile(size);

	if (log_temp_files >= 0)
	{
		if ((size / 1024) >= log_temp_files)
			ereport(LOG,
					(errmsg("temporary file: path \"%s\", size %lu",
							path, (unsigned long) size)));
	}
}

/*
 * Register a file as a temporary file.
 *
 * If 'resowner' is given, the file is registered to be automatically closed
 * by the resource owner.  ResourceOwnerEnlarge(resowner) must have been
 * called before the file was opened.
 *
 * 'flags' indicate whether the file is to be deleted on close
 * (FD_DELETE_AT_CLOSE) and whether it counts towards the temp_file_limit
 * (FD_TEMP_FILE_LIMIT)
 */
void
RegisterTemporaryFile(File file, ResourceOwner resowner, unsigned short flags)
{
	if (resowner)
	{
		ResourceOwnerRememberFile(resowner, file);
		VfdCache[file].resowner = resowner;
	}
	VfdCache[file].tempFlags |= flags;
}

/*
 *	Called when we get a shared invalidation message on some relation.
 */
#ifdef NOT_USED
void
FileInvalidate(File file)
{
	Assert(FileIsValid(file));
	if (!FileIsNotOpen(file))
		LruDelete(file);
}
#endif

/*
 * Open a file with PathNameOpenFilePerm() and pass default file mode for the
 * fileMode parameter.
 */
File
PathNameOpenFile(const char *fileName, int fileFlags)
{
	return PathNameOpenFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * open a file in an arbitrary directory
 *
 * NB: if the passed pathname is relative (which it usually is),
 * it will be interpreted relative to the process' working directory
 * (which should always be $PGDATA when this code is running).
 */
File
PathNameOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	char	   *fnamecopy;
	File		file;
	Vfd		   *vfdP;

	DO_DB(elog(LOG, "PathNameOpenFilePerm: %s %x %o",
			   fileName, fileFlags, fileMode));

	/*
	 * We need a malloc'd copy of the file name; fail cleanly if no room.
	 */
	fnamecopy = strdup(fileName);
	if (fnamecopy == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	file = AllocateVfd();
	vfdP = &VfdCache[file];

	/* Close excess kernel FDs. */
	ReleaseLruFiles();

	/*
	 * Descriptors managed by VFDs are implicitly marked O_CLOEXEC.  The
	 * client shouldn't be expected to know which kernel descriptors are
	 * currently open, so it wouldn't make sense for them to be inherited by
	 * executed subprograms.
	 */
	fileFlags |= O_CLOEXEC;

	vfdP->fd = BasicOpenFilePerm(fileName, fileFlags, fileMode);

	if (vfdP->fd < 0)
	{
		int			save_errno = errno;

		FreeVfd(file);
		free(fnamecopy);
		errno = save_errno;
		return -1;
	}
	++nfile;
	DO_DB(elog(LOG, "PathNameOpenFile: success %d",
			   vfdP->fd));

	vfdP->fileName = fnamecopy;
	/* Saved flags are adjusted to be OK for re-opening file */
	vfdP->fileFlags = fileFlags & ~(O_CREAT | O_TRUNC | O_EXCL);
	vfdP->fileMode = fileMode;
	vfdP->fileSize = 0;
	vfdP->tempFlags = 0x0;
	vfdP->resowner = NULL;

	Insert(file);

	return file;
}

/*
 * close a file when done with it
 */
void
FileClose(File file)
{
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileClose: %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	if (!FileIsNotOpen(file))
	{
		pgaio_closing_fd(vfdP->fd);

		/* close the file */
		if (close(vfdP->fd) != 0)
		{
			/*
			 * We may need to panic on failure to close non-temporary files;
			 * see LruDelete.
			 */
			elog(vfdP->tempFlags & FD_TEMP_FILE_LIMIT ? LOG : data_sync_elevel(LOG),
				 "could not close file \"%s\": %m", vfdP->fileName);
		}

		--nfile;
		vfdP->fd = VFD_CLOSED;

		/* remove the file from the lru ring */
		Delete(file);
	}

	if (vfdP->tempFlags & FD_TEMP_FILE_LIMIT)
	{
		/* Subtract its size from current usage (do first in case of error) */
		temporary_files_size -= vfdP->fileSize;
		vfdP->fileSize = 0;
	}

	/*
	 * Delete the file if it was temporary, and make a log entry if wanted
	 */
	if (vfdP->tempFlags & FD_DELETE_AT_CLOSE)
	{
		struct stat filestats;
		int			stat_errno;

		/*
		 * If we get an error, as could happen within the ereport/elog calls,
		 * we'll come right back here during transaction abort.  Reset the
		 * flag to ensure that we can't get into an infinite loop.  This code
		 * is arranged to ensure that the worst-case consequence is failing to
		 * emit log message(s), not failing to attempt the unlink.
		 */
		vfdP->tempFlags &= ~FD_DELETE_AT_CLOSE;


		/* first try the stat() */
		if (stat(vfdP->fileName, &filestats))
			stat_errno = errno;
		else
			stat_errno = 0;

		/* in any case do the unlink */
		if (unlink(vfdP->fileName))
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not delete file \"%s\": %m", vfdP->fileName)));

		/* and last report the stat results */
		if (stat_errno == 0)
			ReportTemporaryFileUsage(vfdP->fileName, filestats.st_size);
		else
		{
			errno = stat_errno;
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", vfdP->fileName)));
		}
	}

	/* Unregister it from the resource owner */
	if (vfdP->resowner)
		ResourceOwnerForgetFile(vfdP->resowner, file);

	/*
	 * Return the Vfd slot to the free list
	 */
	FreeVfd(file);
}

/*
 * FilePrefetch - initiate asynchronous read of a given range of the file.
 *
 * Returns 0 on success, otherwise an errno error code (like posix_fadvise()).
 *
 * posix_fadvise() is the simplest standardized interface that accomplishes
 * this.
 */
int
FilePrefetch(File file, pgoff_t offset, pgoff_t amount, uint32 wait_event_info)
{
	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FilePrefetch: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_WILLNEED)
	{
		int			returnCode;

		returnCode = FileAccess(file);
		if (returnCode < 0)
			return returnCode;

retry:
		pgstat_report_wait_start(wait_event_info);
		returnCode = posix_fadvise(VfdCache[file].fd, offset, amount,
								   POSIX_FADV_WILLNEED);
		pgstat_report_wait_end();

		if (returnCode == EINTR)
			goto retry;

		return returnCode;
	}
#elif defined(__darwin__)
	{
		struct radvisory
		{
			off_t		ra_offset;	/* offset into the file */
			int			ra_count;	/* size of the read     */
		}			ra;
		int			returnCode;

		returnCode = FileAccess(file);
		if (returnCode < 0)
			return returnCode;

		ra.ra_offset = offset;
		ra.ra_count = amount;
		pgstat_report_wait_start(wait_event_info);
		returnCode = fcntl(VfdCache[file].fd, F_RDADVISE, &ra);
		pgstat_report_wait_end();
		if (returnCode != -1)
			return 0;
		else
			return errno;
	}
#else
	return 0;
#endif
}

void
FileWriteback(File file, pgoff_t offset, pgoff_t nbytes, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileWriteback: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) nbytes));

	if (nbytes <= 0)
		return;

	if (VfdCache[file].fileFlags & PG_O_DIRECT)
		return;

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return;

	pgstat_report_wait_start(wait_event_info);
	pg_flush_data(VfdCache[file].fd, offset, nbytes);
	pgstat_report_wait_end();
}

ssize_t
FileReadV(File file, const struct iovec *iov, int iovcnt, pgoff_t offset,
		  uint32 wait_event_info)
{
	ssize_t		returnCode;
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileReadV: %d (%s) " INT64_FORMAT " %d",
			   file, VfdCache[file].fileName,
			   (int64) offset,
			   iovcnt));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	vfdP = &VfdCache[file];

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_preadv(vfdP->fd, iov, iovcnt, offset);
	pgstat_report_wait_end();

	if (returnCode < 0)
	{
		/*
		 * Windows may run out of kernel buffers and return "Insufficient
		 * system resources" error.  Wait a bit and retry to solve it.
		 *
		 * It is rumored that EINTR is also possible on some Unix filesystems,
		 * in which case immediate retry is indicated.
		 */
#ifdef WIN32
		DWORD		error = GetLastError();

		switch (error)
		{
			case ERROR_NO_SYSTEM_RESOURCES:
				pg_usleep(1000L);
				errno = EINTR;
				break;
			default:
				_dosmaperr(error);
				break;
		}
#endif
		/* OK to retry if interrupted */
		if (errno == EINTR)
			goto retry;
	}

	return returnCode;
}

int
FileStartReadV(PgAioHandle *ioh, File file,
			   int iovcnt, pgoff_t offset,
			   uint32 wait_event_info)
{
	int			returnCode;
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileStartReadV: %d (%s) " INT64_FORMAT " %d",
			   file, VfdCache[file].fileName,
			   (int64) offset,
			   iovcnt));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	vfdP = &VfdCache[file];

	pgaio_io_start_readv(ioh, vfdP->fd, iovcnt, offset);

	return 0;
}

ssize_t
FileWriteV(File file, const struct iovec *iov, int iovcnt, pgoff_t offset,
		   uint32 wait_event_info)
{
	ssize_t		returnCode;
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileWriteV: %d (%s) " INT64_FORMAT " %d",
			   file, VfdCache[file].fileName,
			   (int64) offset,
			   iovcnt));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	vfdP = &VfdCache[file];

	/*
	 * If enforcing temp_file_limit and it's a temp file, check to see if the
	 * write would overrun temp_file_limit, and throw error if so.  Note: it's
	 * really a modularity violation to throw error here; we should set errno
	 * and return -1.  However, there's no way to report a suitable error
	 * message if we do that.  All current callers would just throw error
	 * immediately anyway, so this is safe at present.
	 */
	if (temp_file_limit >= 0 && (vfdP->tempFlags & FD_TEMP_FILE_LIMIT))
	{
		pgoff_t		past_write = offset;

		for (int i = 0; i < iovcnt; ++i)
			past_write += iov[i].iov_len;

		if (past_write > vfdP->fileSize)
		{
			uint64		newTotal = temporary_files_size;

			newTotal += past_write - vfdP->fileSize;
			if (newTotal > (uint64) temp_file_limit * (uint64) 1024)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
						 errmsg("temporary file size exceeds \"temp_file_limit\" (%dkB)",
								temp_file_limit)));
		}
	}

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_pwritev(vfdP->fd, iov, iovcnt, offset);
	pgstat_report_wait_end();

	if (returnCode >= 0)
	{
		/*
		 * Some callers expect short writes to set errno, and traditionally we
		 * have assumed that they imply disk space shortage.  We don't want to
		 * waste CPU cycles adding up the total size here, so we'll just set
		 * it for all successful writes in case such a caller determines that
		 * the write was short and ereports "%m".
		 */
		errno = ENOSPC;

		/*
		 * Maintain fileSize and temporary_files_size if it's a temp file.
		 */
		if (vfdP->tempFlags & FD_TEMP_FILE_LIMIT)
		{
			pgoff_t		past_write = offset + returnCode;

			if (past_write > vfdP->fileSize)
			{
				temporary_files_size += past_write - vfdP->fileSize;
				vfdP->fileSize = past_write;
			}
		}
	}
	else
	{
		/*
		 * See comments in FileReadV()
		 */
#ifdef WIN32
		DWORD		error = GetLastError();

		switch (error)
		{
			case ERROR_NO_SYSTEM_RESOURCES:
				pg_usleep(1000L);
				errno = EINTR;
				break;
			default:
				_dosmaperr(error);
				break;
		}
#endif
		/* OK to retry if interrupted */
		if (errno == EINTR)
			goto retry;
	}

	return returnCode;
}

int
FileSync(File file, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileSync: %d (%s)",
			   file, VfdCache[file].fileName));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_fsync(VfdCache[file].fd);
	pgstat_report_wait_end();

	return returnCode;
}

/*
 * Zero a region of the file.
 *
 * Returns 0 on success, -1 otherwise. In the latter case errno is set to the
 * appropriate error.
 */
int
FileZero(File file, pgoff_t offset, pgoff_t amount, uint32 wait_event_info)
{
	int			returnCode;
	ssize_t		written;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileZero: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	written = pg_pwrite_zeros(VfdCache[file].fd, amount, offset);
	pgstat_report_wait_end();

	if (written < 0)
		return -1;
	else if (written != amount)
	{
		/* if errno is unset, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		return -1;
	}

	return 0;
}

/*
 * Try to reserve file space with posix_fallocate(). If posix_fallocate() is
 * not implemented on the operating system or fails with EINVAL / EOPNOTSUPP,
 * use FileZero() instead.
 *
 * Note that at least glibc() implements posix_fallocate() in userspace if not
 * implemented by the filesystem. That's not the case for all environments
 * though.
 *
 * Returns 0 on success, -1 otherwise. In the latter case errno is set to the
 * appropriate error.
 */
int
FileFallocate(File file, pgoff_t offset, pgoff_t amount, uint32 wait_event_info)
{
#ifdef HAVE_POSIX_FALLOCATE
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileFallocate: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return -1;

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = posix_fallocate(VfdCache[file].fd, offset, amount);
	pgstat_report_wait_end();

	if (returnCode == 0)
		return 0;
	else if (returnCode == EINTR)
		goto retry;

	/* for compatibility with %m printing etc */
	errno = returnCode;

	/*
	 * Return in cases of a "real" failure, if fallocate is not supported,
	 * fall through to the FileZero() backed implementation.
	 */
	if (returnCode != EINVAL && returnCode != EOPNOTSUPP)
		return -1;
#endif

	return FileZero(file, offset, amount, wait_event_info);
}

pgoff_t
FileSize(File file)
{
	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileSize %d (%s)",
			   file, VfdCache[file].fileName));

	if (FileIsNotOpen(file))
	{
		if (FileAccess(file) < 0)
			return (pgoff_t) -1;
	}

	return lseek(VfdCache[file].fd, 0, SEEK_END);
}

int
FileTruncate(File file, pgoff_t offset, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileTruncate %d (%s)",
			   file, VfdCache[file].fileName));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_ftruncate(VfdCache[file].fd, offset);
	pgstat_report_wait_end();

	if (returnCode == 0 && VfdCache[file].fileSize > offset)
	{
		/* adjust our state for truncation of a temp file */
		Assert(VfdCache[file].tempFlags & FD_TEMP_FILE_LIMIT);
		temporary_files_size -= VfdCache[file].fileSize - offset;
		VfdCache[file].fileSize = offset;
	}

	return returnCode;
}

/*
 * Return the pathname associated with an open file.
 *
 * The returned string points to an internal buffer, which is valid until
 * the file is closed.
 */
char *
FilePathName(File file)
{
	Assert(FileIsValid(file));

	return VfdCache[file].fileName;
}

/*
 * Return the raw file descriptor of an opened file.
 *
 * The returned file descriptor will be valid until the file is closed, but
 * there are a lot of things that can make that happen.  So the caller should
 * be careful not to do much of anything else before it finishes using the
 * returned file descriptor.
 */
int
FileGetRawDesc(File file)
{
	int			returnCode;

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	Assert(FileIsValid(file));
	return VfdCache[file].fd;
}

/*
 * FileGetRawFlags - returns the file flags on open(2)
 */
int
FileGetRawFlags(File file)
{
	Assert(FileIsValid(file));
	return VfdCache[file].fileFlags;
}

/*
 * FileGetRawMode - returns the mode bitmask passed to open(2)
 */
mode_t
FileGetRawMode(File file)
{
	Assert(FileIsValid(file));
	return VfdCache[file].fileMode;
}

/*
 * Make room for another allocatedDescs[] array entry if needed and possible.
 * Returns true if an array element is available.
 */
static bool
reserveAllocatedDesc(void)
{
	AllocateDesc *newDescs;
	int			newMax;

	/* Quick out if array already has a free slot. */
	if (numAllocatedDescs < maxAllocatedDescs)
		return true;

	/*
	 * If the array hasn't yet been created in the current process, initialize
	 * it with FD_MINFREE / 3 elements.  In many scenarios this is as many as
	 * we will ever need, anyway.  We don't want to look at max_safe_fds
	 * immediately because set_max_safe_fds() may not have run yet.
	 */
	if (allocatedDescs == NULL)
	{
		newMax = FD_MINFREE / 3;
		newDescs = (AllocateDesc *) malloc(newMax * sizeof(AllocateDesc));
		/* Out of memory already?  Treat as fatal error. */
		if (newDescs == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		allocatedDescs = newDescs;
		maxAllocatedDescs = newMax;
		return true;
	}

	/*
	 * Consider enlarging the array beyond the initial allocation used above.
	 * By the time this happens, max_safe_fds should be known accurately.
	 *
	 * We mustn't let allocated descriptors hog all the available FDs, and in
	 * practice we'd better leave a reasonable number of FDs for VFD use.  So
	 * set the maximum to max_safe_fds / 3.  (This should certainly be at
	 * least as large as the initial size, FD_MINFREE / 3, so we aren't
	 * tightening the restriction here.)  Recall that "external" FDs are
	 * allowed to consume another third of max_safe_fds.
	 */
	newMax = max_safe_fds / 3;
	if (newMax > maxAllocatedDescs)
	{
		newDescs = (AllocateDesc *) realloc(allocatedDescs,
											newMax * sizeof(AllocateDesc));
		/* Treat out-of-memory as a non-fatal error. */
		if (newDescs == NULL)
			return false;
		allocatedDescs = newDescs;
		maxAllocatedDescs = newMax;
		return true;
	}

	/* Can't enlarge allocatedDescs[] any more. */
	return false;
}

/*
 * Routines that want to use stdio (ie, FILE*) should use AllocateFile
 * rather than plain fopen().  This lets fd.c deal with freeing FDs if
 * necessary to open the file.  When done, call FreeFile rather than fclose.
 *
 * Note that files that will be open for any significant length of time
 * should NOT be handled this way, since they cannot share kernel file
 * descriptors with other files; there is grave risk of running out of FDs
 * if anyone locks down too many FDs.  Most callers of this routine are
 * simply reading a config file that they will read and close immediately.
 *
 * fd.c will automatically close all files opened with AllocateFile at
 * transaction commit or abort; this prevents FD leakage if a routine
 * that calls AllocateFile is terminated prematurely by ereport(ERROR).
 *
 * Ideally this should be the *only* direct call of fopen() in the backend.
 */
FILE *
AllocateFile(const char *name, const char *mode)
{
	FILE	   *file;

	DO_DB(elog(LOG, "AllocateFile: Allocated %d (%s)",
			   numAllocatedDescs, name));

	/* Can we allocate another non-virtual FD? */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open file \"%s\"",
						maxAllocatedDescs, name)));

	/* Close excess kernel FDs. */
	ReleaseLruFiles();

TryAgain:
	if ((file = fopen(name, mode)) != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescFile;
		desc->desc.file = file;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.file;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * Open a file with OpenTransientFilePerm() and pass default file mode for
 * the fileMode parameter.
 */
int
OpenTransientFile(const char *fileName, int fileFlags)
{
	return OpenTransientFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * Like AllocateFile, but returns an unbuffered fd like open(2)
 */
int
OpenTransientFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	int			fd;

	DO_DB(elog(LOG, "OpenTransientFile: Allocated %d (%s)",
			   numAllocatedDescs, fileName));

	/* Can we allocate another non-virtual FD? */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open file \"%s\"",
						maxAllocatedDescs, fileName)));

	/* Close excess kernel FDs. */
	ReleaseLruFiles();

	fd = BasicOpenFilePerm(fileName, fileFlags, fileMode);

	if (fd >= 0)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescRawFD;
		desc->desc.fd = fd;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;

		return fd;
	}

	return -1;					/* failure */
}

/*
 * Routines that want to initiate a pipe stream should use OpenPipeStream
 * rather than plain popen().  This lets fd.c deal with freeing FDs if
 * necessary.  When done, call ClosePipeStream rather than pclose.
 *
 * This function also ensures that the popen'd program is run with default
 * SIGPIPE processing, rather than the SIG_IGN setting the backend normally
 * uses.  This ensures desirable response to, eg, closing a read pipe early.
 */
FILE *
OpenPipeStream(const char *command, const char *mode)
{
	FILE	   *file;
	int			save_errno;

	DO_DB(elog(LOG, "OpenPipeStream: Allocated %d (%s)",
			   numAllocatedDescs, command));

	/* Can we allocate another non-virtual FD? */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to execute command \"%s\"",
						maxAllocatedDescs, command)));

	/* Close excess kernel FDs. */
	ReleaseLruFiles();

TryAgain:
	fflush(NULL);
	pqsignal(SIGPIPE, PG_SIG_DFL);
	errno = 0;
	file = popen(command, mode);
	save_errno = errno;
	pqsignal(SIGPIPE, PG_SIG_IGN);
	errno = save_errno;
	if (file != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescPipe;
		desc->desc.file = file;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.file;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * Free an AllocateDesc of any type.
 *
 * The argument *must* point into the allocatedDescs[] array.
 */
static int
FreeDesc(AllocateDesc *desc)
{
	int			result;

	/* Close the underlying object */
	switch (desc->kind)
	{
		case AllocateDescFile:
			result = fclose(desc->desc.file);
			break;
		case AllocateDescPipe:
			result = pclose(desc->desc.file);
			break;
		case AllocateDescDir:
			result = closedir(desc->desc.dir);
			break;
		case AllocateDescRawFD:
			pgaio_closing_fd(desc->desc.fd);
			result = close(desc->desc.fd);
			break;
		default:
			elog(ERROR, "AllocateDesc kind not recognized");
			result = 0;			/* keep compiler quiet */
			break;
	}

	/* Compact storage in the allocatedDescs array */
	numAllocatedDescs--;
	*desc = allocatedDescs[numAllocatedDescs];

	return result;
}

/*
 * Close a file returned by AllocateFile.
 *
 * Note we do not check fclose's return value --- it is up to the caller
 * to handle close errors.
 */
int
FreeFile(FILE *file)
{
	int			i;

	DO_DB(elog(LOG, "FreeFile: Allocated %d", numAllocatedDescs));

	/* Remove file from list of allocated files, if it's present */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescFile && desc->desc.file == file)
			return FreeDesc(desc);
	}

	/* Only get here if someone passes us a file not in allocatedDescs */
	elog(WARNING, "file passed to FreeFile was not obtained from AllocateFile");

	return fclose(file);
}

/*
 * Close a file returned by OpenTransientFile.
 *
 * Note we do not check close's return value --- it is up to the caller
 * to handle close errors.
 */
int
CloseTransientFile(int fd)
{
	int			i;

	DO_DB(elog(LOG, "CloseTransientFile: Allocated %d", numAllocatedDescs));

	/* Remove fd from list of allocated files, if it's present */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescRawFD && desc->desc.fd == fd)
			return FreeDesc(desc);
	}

	/* Only get here if someone passes us a file not in allocatedDescs */
	elog(WARNING, "fd passed to CloseTransientFile was not obtained from OpenTransientFile");

	pgaio_closing_fd(fd);

	return close(fd);
}

/*
 * Routines that want to use <dirent.h> (ie, DIR*) should use AllocateDir
 * rather than plain opendir().  This lets fd.c deal with freeing FDs if
 * necessary to open the directory, and with closing it after an elog.
 * When done, call FreeDir rather than closedir.
 *
 * Returns NULL, with errno set, on failure.  Note that failure detection
 * is commonly left to the following call of ReadDir or ReadDirExtended;
 * see the comments for ReadDir.
 *
 * Ideally this should be the *only* direct call of opendir() in the backend.
 */
DIR *
AllocateDir(const char *dirname)
{
	DIR		   *dir;

	DO_DB(elog(LOG, "AllocateDir: Allocated %d (%s)",
			   numAllocatedDescs, dirname));

	/* Can we allocate another non-virtual FD? */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open directory \"%s\"",
						maxAllocatedDescs, dirname)));

	/* Close excess kernel FDs. */
	ReleaseLruFiles();

TryAgain:
	if ((dir = opendir(dirname)) != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescDir;
		desc->desc.dir = dir;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.dir;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * Read a directory opened with AllocateDir, ereport'ing any error.
 *
 * This is easier to use than raw readdir() since it takes care of some
 * otherwise rather tedious and error-prone manipulation of errno.  Also,
 * if you are happy with a generic error message for AllocateDir failure,
 * you can just do
 *
 *		dir = AllocateDir(path);
 *		while ((dirent = ReadDir(dir, path)) != NULL)
 *			process dirent;
 *		FreeDir(dir);
 *
 * since a NULL dir parameter is taken as indicating AllocateDir failed.
 * (Make sure errno isn't changed between AllocateDir and ReadDir if you
 * use this shortcut.)
 *
 * The pathname passed to AllocateDir must be passed to this routine too,
 * but it is only used for error reporting.
 */
struct dirent *
ReadDir(DIR *dir, const char *dirname)
{
	return ReadDirExtended(dir, dirname, ERROR);
}

/*
 * Alternate version of ReadDir that allows caller to specify the elevel
 * for any error report (whether it's reporting an initial failure of
 * AllocateDir or a subsequent directory read failure).
 *
 * If elevel < ERROR, returns NULL after any error.  With the normal coding
 * pattern, this will result in falling out of the loop immediately as
 * though the directory contained no (more) entries.
 */
struct dirent *
ReadDirExtended(DIR *dir, const char *dirname, int elevel)
{
	struct dirent *dent;

	/* Give a generic message for AllocateDir failure, if caller didn't */
	if (dir == NULL)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						dirname)));
		return NULL;
	}

	errno = 0;
	if ((dent = readdir(dir)) != NULL)
		return dent;

	if (errno)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not read directory \"%s\": %m",
						dirname)));
	return NULL;
}

/*
 * Close a directory opened with AllocateDir.
 *
 * Returns closedir's return value (with errno set if it's not 0).
 * Note we do not check the return value --- it is up to the caller
 * to handle close errors if wanted.
 *
 * Does nothing if dir == NULL; we assume that directory open failure was
 * already reported if desired.
 */
int
FreeDir(DIR *dir)
{
	int			i;

	/* Nothing to do if AllocateDir failed */
	if (dir == NULL)
		return 0;

	DO_DB(elog(LOG, "FreeDir: Allocated %d", numAllocatedDescs));

	/* Remove dir from list of allocated dirs, if it's present */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescDir && desc->desc.dir == dir)
			return FreeDesc(desc);
	}

	/* Only get here if someone passes us a dir not in allocatedDescs */
	elog(WARNING, "dir passed to FreeDir was not obtained from AllocateDir");

	return closedir(dir);
}


/*
 * Close a pipe stream returned by OpenPipeStream.
 */
int
ClosePipeStream(FILE *file)
{
	int			i;

	DO_DB(elog(LOG, "ClosePipeStream: Allocated %d", numAllocatedDescs));

	/* Remove file from list of allocated files, if it's present */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescPipe && desc->desc.file == file)
			return FreeDesc(desc);
	}

	/* Only get here if someone passes us a file not in allocatedDescs */
	elog(WARNING, "file passed to ClosePipeStream was not obtained from OpenPipeStream");

	return pclose(file);
}

/*
 * closeAllVfds
 *
 * Force all VFDs into the physically-closed state, so that the fewest
 * possible number of kernel file descriptors are in use.  There is no
 * change in the logical state of the VFDs.
 */
void
closeAllVfds(void)
{
	Index		i;

	if (SizeVfdCache > 0)
	{
		Assert(FileIsNotOpen(0));	/* Make sure ring not corrupted */
		for (i = 1; i < SizeVfdCache; i++)
		{
			if (!FileIsNotOpen(i))
				LruDelete(i);
		}
	}
}

/*
 * AtEOSubXact_Files
 *
 * Take care of subtransaction commit/abort.  At abort, we close AllocateDescs
 * that the subtransaction may have opened.  At commit, we reassign them to
 * the parent subtransaction.  (Temporary files are tracked by ResourceOwners
 * instead.)
 */
void
AtEOSubXact_Files(bool isCommit, SubTransactionId mySubid,
				  SubTransactionId parentSubid)
{
	Index		i;

	for (i = 0; i < numAllocatedDescs; i++)
	{
		if (allocatedDescs[i].create_subid == mySubid)
		{
			if (isCommit)
				allocatedDescs[i].create_subid = parentSubid;
			else
			{
				/* have to recheck the item after FreeDesc (ugly) */
				FreeDesc(&allocatedDescs[i--]);
			}
		}
	}
}

/*
 * AtEOXact_Files
 *
 * This routine is called during transaction commit or abort.  All "allocated"
 * stdio files are closed.
 *
 * The isCommit flag is used only to decide whether to emit warnings about
 * unclosed files.
 */
void
AtEOXact_Files(bool isCommit)
{
	/* Complain if any allocated files remain open at commit. */
	if (isCommit && numAllocatedDescs > 0)
		elog(WARNING, "%d temporary files and directories not closed at end-of-transaction",
			 numAllocatedDescs);

	/* Clean up "allocated" stdio files, dirs and fds. */
	while (numAllocatedDescs > 0)
		FreeDesc(&allocatedDescs[0]);
}

/*
 * Close temporary files and delete their underlying files.
 *
 * This is called when the backend process is exiting.  We remove any
 * remaining temporary files that haven't been closed by resource manager
 * cleanup already.
 */
void
CleanupTempFiles(void)
{
	Index		i;

	Assert(FileIsNotOpen(0));	/* Make sure ring not corrupted */
	for (i = 1; i < SizeVfdCache; i++)
	{
		uint16		tempFlags = VfdCache[i].tempFlags;

		if ((tempFlags & FD_DELETE_AT_CLOSE) && VfdCache[i].fileName != NULL)
			FileClose(i);
	}
}

bool
check_debug_io_direct(char **newval, void **extra, GucSource source)
{
	bool		result = true;
	int			flags;

#if PG_O_DIRECT == 0
	if (strcmp(*newval, "") != 0)
	{
		GUC_check_errdetail("\"%s\" is not supported on this platform.",
							"debug_io_direct");
		result = false;
	}
	flags = 0;
#else
	List	   *elemlist;
	ListCell   *l;
	char	   *rawstring;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(*newval);

	if (!SplitGUCList(rawstring, ',', &elemlist))
	{
		GUC_check_errdetail("Invalid list syntax in parameter \"%s\".",
							"debug_io_direct");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	flags = 0;
	foreach(l, elemlist)
	{
		char	   *item = (char *) lfirst(l);

		if (pg_strcasecmp(item, "data") == 0)
			flags |= IO_DIRECT_DATA;
		else if (pg_strcasecmp(item, "wal") == 0)
			flags |= IO_DIRECT_WAL;
		else if (pg_strcasecmp(item, "wal_init") == 0)
			flags |= IO_DIRECT_WAL_INIT;
		else
		{
			GUC_check_errdetail("Invalid option \"%s\".", item);
			result = false;
			break;
		}
	}

	/*
	 * It's possible to configure block sizes smaller than our assumed I/O
	 * alignment size, which could result in invalid I/O requests.
	 */
#if XLOG_BLCKSZ < PG_IO_ALIGN_SIZE
	if (result && (flags & (IO_DIRECT_WAL | IO_DIRECT_WAL_INIT)))
	{
		GUC_check_errdetail("\"%s\" is not supported for WAL because %s is too small.",
							"debug_io_direct", "XLOG_BLCKSZ");
		result = false;
	}
#endif
#if BLCKSZ < PG_IO_ALIGN_SIZE
	if (result && (flags & IO_DIRECT_DATA))
	{
		GUC_check_errdetail("\"%s\" is not supported for data because %s is too small.",
							"debug_io_direct", "BLCKSZ");
		result = false;
	}
#endif

	pfree(rawstring);
	list_free(elemlist);
#endif

	if (!result)
		return result;

	/* Save the flags in *extra, for use by assign_debug_io_direct */
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	*((int *) *extra) = flags;

	return result;
}

void
assign_debug_io_direct(const char *newval, void *extra)
{
	int		   *flags = (int *) extra;

	io_direct_flags = *flags;
}

/* ResourceOwner callbacks */

static void
ResOwnerReleaseFile(Datum res)
{
	File		file = (File) DatumGetInt32(res);
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	vfdP = &VfdCache[file];
	vfdP->resowner = NULL;

	FileClose(file);
}

static char *
ResOwnerPrintFile(Datum res)
{
	return psprintf("File %d", DatumGetInt32(res));
}
