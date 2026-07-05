/*-------------------------------------------------------------------------
 *
 * file_utils.c
 *	  Assorted portability and utility functions to work on files
 *
 * There's a file with the same name in src/common.  It contains
 * frontend versions of some of these functions.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/file_utils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <fcntl.h>

#include "access/xlog.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/file_utils.h"

/* GUC: Whether it is safe to continue running after fsync() fails. */
bool		data_sync_retry = false;

static int	fsync_parent_path(const char *fname, int elevel);

/*
 * Return the passed-in error level, or PANIC if data_sync_retry is off.
 *
 * Failure to fsync any data file is cause for immediate panic, unless
 * data_sync_retry is enabled.  Data may have been written to the operating
 * system and removed from our buffer pool already, and if we are running on
 * an operating system that forgets dirty data on write-back failure, there
 * may be only one copy of the data remaining: in the WAL.  A later attempt to
 * fsync again might falsely report success.  Therefore we must not allow any
 * further checkpoints to be attempted.  data_sync_retry can in theory be
 * enabled on systems known not to drop dirty buffered data on write-back
 * failure (with the likely outcome that checkpoints will continue to fail
 * until the underlying problem is fixed).
 *
 * Any code that reports a failure from fsync() or related functions should
 * filter the error level with this function.
 */
int
data_sync_elevel(int elevel)
{
	return data_sync_retry ? elevel : PANIC;
}

/*
 * pg_fsync --- do fsync with or without writethrough
 */
int
pg_fsync(int fd)
{
#if !defined(WIN32) && defined(USE_ASSERT_CHECKING)
	struct stat st;

	/*
	 * Some operating system implementations of fsync() have requirements
	 * about the file access modes that were used when their file descriptor
	 * argument was opened, and these requirements differ depending on whether
	 * the file descriptor is for a directory.
	 *
	 * For any file descriptor that may eventually be handed to fsync(), we
	 * should have opened it with access modes that are compatible with
	 * fsync() on all supported systems, otherwise the code may not be
	 * portable, even if it runs ok on the current system.
	 *
	 * We assert here that a descriptor for a file was opened with write
	 * permissions (i.e., not O_RDONLY) and for a directory without write
	 * permissions (O_RDONLY).  Notice that the assertion check is made even
	 * if fsync() is disabled.
	 *
	 * If fstat() fails, ignore it and let the follow-up fsync() complain.
	 */
	if (fstat(fd, &st) == 0)
	{
		int			desc_flags = fcntl(fd, F_GETFL);

		desc_flags &= O_ACCMODE;

		if (S_ISDIR(st.st_mode))
			Assert(desc_flags == O_RDONLY);
		else
			Assert(desc_flags != O_RDONLY);
	}
	errno = 0;
#endif

	/* #if is to skip the wal_sync_method test if there's no need for it */
#if defined(HAVE_FSYNC_WRITETHROUGH)
	if (wal_sync_method == WAL_SYNC_METHOD_FSYNC_WRITETHROUGH)
		return pg_fsync_writethrough(fd);
	else
#endif
		return pg_fsync_no_writethrough(fd);
}


/*
 * pg_fsync_no_writethrough --- same as fsync except does nothing if
 *	enableFsync is off
 */
int
pg_fsync_no_writethrough(int fd)
{
	int			rc;

	if (!enableFsync)
		return 0;

retry:
	rc = fsync(fd);

	if (rc == -1 && errno == EINTR)
		goto retry;

	return rc;
}

/*
 * pg_fsync_writethrough
 */
int
pg_fsync_writethrough(int fd)
{
	if (enableFsync)
	{
#if defined(F_FULLFSYNC)
		return (fcntl(fd, F_FULLFSYNC, 0) == -1) ? -1 : 0;
#else
		errno = ENOSYS;
		return -1;
#endif
	}
	else
		return 0;
}

/*
 * pg_fdatasync --- same as fdatasync except does nothing if enableFsync is off
 */
int
pg_fdatasync(int fd)
{
	int			rc;

	if (!enableFsync)
		return 0;

retry:
	rc = fdatasync(fd);

	if (rc == -1 && errno == EINTR)
		goto retry;

	return rc;
}

/*
 * pg_file_exists -- check that a file exists.
 *
 * This requires an absolute path to the file.  Returns true if the file is
 * not a directory, false otherwise.
 */
bool
pg_file_exists(const char *name)
{
	struct stat st;

	Assert(name != NULL);

	if (stat(name, &st) == 0)
		return !S_ISDIR(st.st_mode);
	else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not access file \"%s\": %m", name)));

	return false;
}

/*
 * pg_flush_data --- advise OS that the described dirty data should be flushed
 *
 * offset of 0 with nbytes 0 means that the entire file should be flushed
 */
void
pg_flush_data(int fd, pgoff_t offset, pgoff_t nbytes)
{
	/*
	 * Right now file flushing is primarily used to avoid making later
	 * fsync()/fdatasync() calls have less impact. Thus don't trigger flushes
	 * if fsyncs are disabled - that's a decision we might want to make
	 * configurable at some point.
	 */
	if (!enableFsync)
		return;

	/*
	 * We compile all alternatives that are supported on the current platform,
	 * to find portability problems more easily.
	 */
#if defined(HAVE_SYNC_FILE_RANGE)
	{
		int			rc;
		static bool not_implemented_by_kernel = false;

		if (not_implemented_by_kernel)
			return;

retry:

		/*
		 * sync_file_range(SYNC_FILE_RANGE_WRITE), currently linux specific,
		 * tells the OS that writeback for the specified blocks should be
		 * started, but that we don't want to wait for completion.  Note that
		 * this call might block if too much dirty data exists in the range.
		 * This is the preferable method on OSs supporting it, as it works
		 * reliably when available (contrast to msync()) and doesn't flush out
		 * clean data (like FADV_DONTNEED).
		 */
		rc = sync_file_range(fd, offset, nbytes,
							 SYNC_FILE_RANGE_WRITE);
		if (rc != 0)
		{
			int			elevel;

			if (errno == EINTR)
				goto retry;

			/*
			 * For systems that don't have an implementation of
			 * sync_file_range() such as Windows WSL, generate only one
			 * warning and then suppress all further attempts by this process.
			 */
			if (errno == ENOSYS)
			{
				elevel = WARNING;
				not_implemented_by_kernel = true;
			}
			else
				elevel = data_sync_elevel(WARNING);

			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not flush dirty data: %m")));
		}

		return;
	}
#endif
#if !defined(WIN32) && defined(MS_ASYNC)
	{
		void	   *p;
		static int	pagesize = 0;

		/*
		 * On several OSs msync(MS_ASYNC) on a mmap'ed file triggers
		 * writeback. On linux it only does so if MS_SYNC is specified, but
		 * then it does the writeback synchronously. Luckily all common linux
		 * systems have sync_file_range().  This is preferable over
		 * FADV_DONTNEED because it doesn't flush out clean data.
		 *
		 * We map the file (mmap()), tell the kernel to sync back the contents
		 * (msync()), and then remove the mapping again (munmap()).
		 */

		/* mmap() needs actual length if we want to map whole file */
		if (offset == 0 && nbytes == 0)
		{
			nbytes = lseek(fd, 0, SEEK_END);
			if (nbytes < 0)
			{
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not determine dirty data size: %m")));
				return;
			}
		}

		/*
		 * Some platforms reject partial-page mmap() attempts.  To deal with
		 * that, just truncate the request to a page boundary.  If any extra
		 * bytes don't get flushed, well, it's only a hint anyway.
		 */

		/* fetch pagesize only once */
		if (pagesize == 0)
			pagesize = sysconf(_SC_PAGESIZE);

		/* align length to pagesize, dropping any fractional page */
		if (pagesize > 0)
			nbytes = (nbytes / pagesize) * pagesize;

		/* fractional-page request is a no-op */
		if (nbytes <= 0)
			return;

		/*
		 * mmap could well fail, particularly on 32-bit platforms where there
		 * may simply not be enough address space.  If so, silently fall
		 * through to the next implementation.
		 */
		if (nbytes <= (pgoff_t) SSIZE_MAX)
			p = mmap(NULL, nbytes, PROT_READ, MAP_SHARED, fd, offset);
		else
			p = MAP_FAILED;

		if (p != MAP_FAILED)
		{
			int			rc;

			rc = msync(p, (size_t) nbytes, MS_ASYNC);
			if (rc != 0)
			{
				ereport(data_sync_elevel(WARNING),
						(errcode_for_file_access(),
						 errmsg("could not flush dirty data: %m")));
				/* NB: need to fall through to munmap()! */
			}

			rc = munmap(p, (size_t) nbytes);
			if (rc != 0)
			{
				/* FATAL error because mapping would remain */
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not munmap() while flushing data: %m")));
			}

			return;
		}
	}
#endif
#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
	{
		int			rc;

		/*
		 * Signal the kernel that the passed in range should not be cached
		 * anymore. This has the, desired, side effect of writing out dirty
		 * data, and the, undesired, side effect of likely discarding useful
		 * clean cached blocks.  For the latter reason this is the least
		 * preferable method.
		 */

		rc = posix_fadvise(fd, offset, nbytes, POSIX_FADV_DONTNEED);

		if (rc != 0)
		{
			/* don't error out, this is just a performance optimization */
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not flush dirty data: %m")));
		}

		return;
	}
#endif
}

/*
 * Truncate an open file to a given length.
 */
int
pg_ftruncate(int fd, pgoff_t length)
{
	int			ret;

retry:
	ret = ftruncate(fd, length);

	if (ret == -1 && errno == EINTR)
		goto retry;

	return ret;
}

/*
 * Truncate a file to a given length by name.
 */
int
pg_truncate(const char *path, pgoff_t length)
{
	int			ret;
#ifdef WIN32
	int			save_errno;
	int			fd;

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd >= 0)
	{
		ret = pg_ftruncate(fd, length);
		save_errno = errno;
		CloseTransientFile(fd);
		errno = save_errno;
	}
	else
		ret = -1;
#else

retry:
	ret = truncate(path, length);

	if (ret == -1 && errno == EINTR)
		goto retry;
#endif

	return ret;
}

/*
 * fsync_fname -- fsync a file or directory, handling errors properly
 *
 * Try to fsync a file or directory. When doing the latter, ignore errors that
 * indicate the OS just doesn't allow/require fsyncing directories.
 */
void
fsync_fname(const char *fname, bool isdir)
{
	fsync_fname_ext(fname, isdir, false, data_sync_elevel(ERROR));
}

/*
 * fsync_fname_ext -- Try to fsync a file or directory
 *
 * If ignore_perm is true, ignore errors upon trying to open unreadable
 * files. Logs other errors at a caller-specified level.
 *
 * Returns 0 if the operation succeeded, -1 otherwise.
 */
int
fsync_fname_ext(const char *fname, bool isdir, bool ignore_perm, int elevel)
{
	int			fd;
	int			flags;
	int			returncode;

	/*
	 * Some OSs require directories to be opened read-only whereas other
	 * systems don't allow us to fsync files opened read-only; so we need both
	 * cases here.  Using O_RDWR will cause us to fail to fsync files that are
	 * not writable by our userid, but we assume that's OK.
	 */
	flags = PG_BINARY;
	if (!isdir)
		flags |= O_RDWR;
	else
		flags |= O_RDONLY;

	fd = OpenTransientFile(fname, flags);

	/*
	 * Some OSs don't allow us to open directories at all (Windows returns
	 * EACCES), just ignore the error in that case.  If desired also silently
	 * ignoring errors about unreadable files. Log others.
	 */
	if (fd < 0 && isdir && (errno == EISDIR || errno == EACCES))
		return 0;
	else if (fd < 0 && ignore_perm && errno == EACCES)
		return 0;
	else if (fd < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fname)));
		return -1;
	}

	returncode = pg_fsync(fd);

	/*
	 * Some OSes don't allow us to fsync directories at all, so we can ignore
	 * those errors. Anything else needs to be logged.
	 */
	if (returncode != 0 && !(isdir && (errno == EBADF || errno == EINVAL)))
	{
		int			save_errno;

		/* close file upon error, might not be in transaction context */
		save_errno = errno;
		(void) CloseTransientFile(fd);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", fname)));
		return -1;
	}

	if (CloseTransientFile(fd) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fname)));
		return -1;
	}

	return 0;
}


/*
 * durable_rename -- rename(2) wrapper, issuing fsyncs required for durability
 *
 * This routine ensures that, after returning, the effect of renaming file
 * persists in case of a crash. A crash while this routine is running will
 * leave you with either the pre-existing or the moved file in place of the
 * new file; no mixed state or truncated files are possible.
 *
 * It does so by using fsync on the old filename and the possibly existing
 * target filename before the rename, and the target file and directory after.
 *
 * Note that rename() cannot be used across arbitrary directories, as they
 * might not be on the same filesystem. Therefore this routine does not
 * support renaming across directories.
 *
 * Log errors with the caller specified severity.
 *
 * Returns 0 if the operation succeeded, -1 otherwise. Note that errno is not
 * valid upon return.
 */
int
durable_rename(const char *oldfile, const char *newfile, int elevel)
{
	int			fd;

	/*
	 * First fsync the old and target path (if it exists), to ensure that they
	 * are properly persistent on disk. Syncing the target file is not
	 * strictly necessary, but it makes it easier to reason about crashes;
	 * because it's then guaranteed that either source or target file exists
	 * after a crash.
	 */
	if (fsync_fname_ext(oldfile, false, false, elevel) != 0)
		return -1;

	fd = OpenTransientFile(newfile, PG_BINARY | O_RDWR);
	if (fd < 0)
	{
		if (errno != ENOENT)
		{
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", newfile)));
			return -1;
		}
	}
	else
	{
		if (pg_fsync(fd) != 0)
		{
			int			save_errno;

			/* close file upon error, might not be in transaction context */
			save_errno = errno;
			CloseTransientFile(fd);
			errno = save_errno;

			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m", newfile)));
			return -1;
		}

		if (CloseTransientFile(fd) != 0)
		{
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", newfile)));
			return -1;
		}
	}

	/* Time to do the real deal... */
	if (rename(oldfile, newfile) < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						oldfile, newfile)));
		return -1;
	}

	/*
	 * To guarantee renaming the file is persistent, fsync the file with its
	 * new name, and its containing directory.
	 */
	if (fsync_fname_ext(newfile, false, false, elevel) != 0)
		return -1;

	if (fsync_parent_path(newfile, elevel) != 0)
		return -1;

	return 0;
}

/*
 * durable_unlink -- remove a file in a durable manner
 *
 * This routine ensures that, after returning, the effect of removing file
 * persists in case of a crash. A crash while this routine is running will
 * leave the system in no mixed state.
 *
 * It does so by using fsync on the parent directory of the file after the
 * actual removal is done.
 *
 * Log errors with the severity specified by caller.
 *
 * Returns 0 if the operation succeeded, -1 otherwise. Note that errno is not
 * valid upon return.
 */
int
durable_unlink(const char *fname, int elevel)
{
	if (unlink(fname) < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						fname)));
		return -1;
	}

	/*
	 * To guarantee that the removal of the file is persistent, fsync its
	 * parent directory.
	 */
	if (fsync_parent_path(fname, elevel) != 0)
		return -1;

	return 0;
}

/*
 * fsync_parent_path -- fsync the parent path of a file or directory
 *
 * This is aimed at making file operations persistent on disk in case of
 * an OS crash or power failure.
 */
static int
fsync_parent_path(const char *fname, int elevel)
{
	char		parentpath[MAXPGPATH];

	strlcpy(parentpath, fname, MAXPGPATH);
	get_parent_directory(parentpath);

	/*
	 * get_parent_directory() returns an empty string if the input argument is
	 * just a file name (see comments in path.c), so handle that as being the
	 * current directory.
	 */
	if (strlen(parentpath) == 0)
		strlcpy(parentpath, ".", MAXPGPATH);

	if (fsync_fname_ext(parentpath, true, false, elevel) != 0)
		return -1;

	return 0;
}
