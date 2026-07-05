/*-------------------------------------------------------------------------
 *
 * datadir.c
 *	  Functions to work on the datadir
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/datadir.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>

#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "postmaster/startup.h"
#include "storage/datadir.h"
#include "storage/fd.h"
#include "storage/file_utils.h"
#include "storage/tempfile.h"

/* Define PG_FLUSH_DATA_WORKS if we have an implementation for pg_flush_data */
#if defined(HAVE_SYNC_FILE_RANGE)
#define PG_FLUSH_DATA_WORKS 1
#elif !defined(WIN32) && defined(MS_ASYNC)
#define PG_FLUSH_DATA_WORKS 1
#elif defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
#define PG_FLUSH_DATA_WORKS 1
#endif

/* How SyncDataDirectory() should do its job. */
int			recovery_init_sync_method = DATA_DIR_SYNC_METHOD_FSYNC;

static void walkdir(const char *path,
					void (*action) (const char *fname, bool isdir, int elevel),
					bool process_symlinks,
					int elevel);
#ifdef PG_FLUSH_DATA_WORKS
static void pre_sync_fname(const char *fname, bool isdir, int elevel);
#endif
static void datadir_fsync_fname(const char *fname, bool isdir, int elevel);
static void unlink_if_exists_fname(const char *fname, bool isdir, int elevel);

/*
 * Create a PostgreSQL data sub-directory
 *
 * The data directory itself, and most of its sub-directories, are created at
 * initdb time, but we do have some occasions when we create directories in
 * the backend (CREATE TABLESPACE, for example).  In those cases, we want to
 * make sure that those directories are created consistently.  Today, that means
 * making sure that the created directory has the correct permissions, which is
 * what pg_dir_create_mode tracks for us.
 *
 * Note that we also set the umask() based on what we understand the correct
 * permissions to be (see file_perm.c).
 *
 * For permissions other than the default, mkdir() can be used directly, but
 * be sure to consider carefully such cases -- a sub-directory with incorrect
 * permissions in a PostgreSQL data directory could cause backups and other
 * processes to fail.
 */
int
MakePGDirectory(const char *directoryName)
{
	return mkdir(directoryName, pg_dir_create_mode);
}

/*
 * Create directory 'directory'.  If necessary, create 'basedir', which must
 * be the directory above it.  This is designed for creating the top-level
 * temporary directory on demand before creating a directory underneath it.
 * Do nothing if the directory already exists.
 *
 * Directories created within the top-level temporary directory should begin
 * with PG_TEMP_FILE_PREFIX, so that they can be identified as temporary and
 * deleted at startup by RemovePgTempFiles().  Further subdirectories below
 * that do not need any particular prefix.
 */
void
PathNameCreateTemporaryDir(const char *basedir, const char *directory)
{
	if (MakePGDirectory(directory) < 0)
	{
		if (errno == EEXIST)
			return;

		/*
		 * Failed.  Try to create basedir first in case it's missing. Tolerate
		 * EEXIST to close a race against another process following the same
		 * algorithm.
		 */
		if (MakePGDirectory(basedir) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("cannot create temporary directory \"%s\": %m",
							basedir)));

		/* Try again. */
		if (MakePGDirectory(directory) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("cannot create temporary subdirectory \"%s\": %m",
							directory)));
	}
}

/*
 * Delete a directory and everything in it, if it exists.
 */
void
PathNameDeleteTemporaryDir(const char *dirname)
{
	struct stat statbuf;

	/* Silently ignore missing directory. */
	if (stat(dirname, &statbuf) != 0 && errno == ENOENT)
		return;

	/*
	 * Currently, walkdir doesn't offer a way for our passed in function to
	 * maintain state.  Perhaps it should, so that we could tell the caller
	 * whether this operation succeeded or failed.  Since this operation is
	 * used in a cleanup path, we wouldn't actually behave differently: we'll
	 * just log failures.
	 */
	walkdir(dirname, unlink_if_exists_fname, false, LOG);
}

#ifdef HAVE_SYNCFS
static void
do_syncfs(const char *path)
{
	int			fd;

	ereport_startup_progress("syncing data directory (syncfs), elapsed time: %ld.%02d s, current path: %s",
							 path);

	fd = OpenTransientFile(path, O_RDONLY);
	if (fd < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
		return;
	}
	if (syncfs(fd) < 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not synchronize file system for file \"%s\": %m", path)));
	CloseTransientFile(fd);
}
#endif

/*
 * Issue fsync recursively on PGDATA and all its contents, or issue syncfs for
 * all potential filesystem, depending on recovery_init_sync_method setting.
 *
 * We fsync regular files and directories wherever they are, but we
 * follow symlinks only for pg_wal and immediately under pg_tblspc.
 * Other symlinks are presumed to point at files we're not responsible
 * for fsyncing, and might not have privileges to write at all.
 *
 * Errors are logged but not considered fatal; that's because this is used
 * only during database startup, to deal with the possibility that there are
 * issued-but-unsynced writes pending against the data directory.  We want to
 * ensure that such writes reach disk before anything that's done in the new
 * run.  However, aborting on error would result in failure to start for
 * harmless cases such as read-only files in the data directory, and that's
 * not good either.
 *
 * Note that if we previously crashed due to a PANIC on fsync(), we'll be
 * rewriting all changes again during recovery.
 *
 * Note we assume we're chdir'd into PGDATA to begin with.
 */
void
SyncDataDirectory(void)
{
	bool		xlog_is_symlink;

	/* We can skip this whole thing if fsync is disabled. */
	if (!enableFsync)
		return;

	/*
	 * If pg_wal is a symlink, we'll need to recurse into it separately,
	 * because the first walkdir below will ignore it.
	 */
	xlog_is_symlink = false;

	{
		struct stat st;

		if (lstat("pg_wal", &st) < 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m",
							"pg_wal")));
		else if (S_ISLNK(st.st_mode))
			xlog_is_symlink = true;
	}

#ifdef HAVE_SYNCFS
	if (recovery_init_sync_method == DATA_DIR_SYNC_METHOD_SYNCFS)
	{
		DIR		   *dir;
		struct dirent *de;

		/*
		 * On Linux, we don't have to open every single file one by one.  We
		 * can use syncfs() to sync whole filesystems.  We only expect
		 * filesystem boundaries to exist where we tolerate symlinks, namely
		 * pg_wal and the tablespaces, so we call syncfs() for each of those
		 * directories.
		 */

		/* Prepare to report progress syncing the data directory via syncfs. */
		begin_startup_progress_phase();

		/* Sync the top level pgdata directory. */
		do_syncfs(".");
		/* If any tablespaces are configured, sync each of those. */
		dir = AllocateDir(PG_TBLSPC_DIR);
		while ((de = ReadDirExtended(dir, PG_TBLSPC_DIR, LOG)))
		{
			char		path[MAXPGPATH];

			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;

			snprintf(path, MAXPGPATH, "%s/%s", PG_TBLSPC_DIR, de->d_name);
			do_syncfs(path);
		}
		FreeDir(dir);
		/* If pg_wal is a symlink, process that too. */
		if (xlog_is_symlink)
			do_syncfs("pg_wal");
		return;
	}
#endif							/* !HAVE_SYNCFS */

#ifdef PG_FLUSH_DATA_WORKS
	/* Prepare to report progress of the pre-fsync phase. */
	begin_startup_progress_phase();

	/*
	 * If possible, hint to the kernel that we're soon going to fsync the data
	 * directory and its contents.  Errors in this step are even less
	 * interesting than normal, so log them only at DEBUG1.
	 */
	walkdir(".", pre_sync_fname, false, DEBUG1);
	if (xlog_is_symlink)
		walkdir("pg_wal", pre_sync_fname, false, DEBUG1);
	walkdir(PG_TBLSPC_DIR, pre_sync_fname, true, DEBUG1);
#endif

	/* Prepare to report progress syncing the data directory via fsync. */
	begin_startup_progress_phase();

	/*
	 * Now we do the fsync()s in the same order.
	 *
	 * The main call ignores symlinks, so in addition to specially processing
	 * pg_wal if it's a symlink, pg_tblspc has to be visited separately with
	 * process_symlinks = true.  Note that if there are any plain directories
	 * in pg_tblspc, they'll get fsync'd twice.  That's not an expected case
	 * so we don't worry about optimizing it.
	 */
	walkdir(".", datadir_fsync_fname, false, LOG);
	if (xlog_is_symlink)
		walkdir("pg_wal", datadir_fsync_fname, false, LOG);
	walkdir(PG_TBLSPC_DIR, datadir_fsync_fname, true, LOG);
}

/*
 * walkdir: recursively walk a directory, applying the action to each
 * regular file and directory (including the named directory itself).
 *
 * If process_symlinks is true, the action and recursion are also applied
 * to regular files and directories that are pointed to by symlinks in the
 * given directory; otherwise symlinks are ignored.  Symlinks are always
 * ignored in subdirectories, ie we intentionally don't pass down the
 * process_symlinks flag to recursive calls.
 *
 * Errors are reported at level elevel, which might be ERROR or less.
 *
 * See also walkdir in file_utils.c, which is a frontend version of this
 * logic.
 */
static void
walkdir(const char *path,
		void (*action) (const char *fname, bool isdir, int elevel),
		bool process_symlinks,
		int elevel)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir(path);

	while ((de = ReadDirExtended(dir, path, elevel)) != NULL)
	{
		char		subpath[MAXPGPATH * 2];

		CHECK_FOR_INTERRUPTS();

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);

		switch (get_dirent_type(subpath, de, process_symlinks, elevel))
		{
			case PGFILETYPE_REG:
				(*action) (subpath, false, elevel);
				break;
			case PGFILETYPE_DIR:
				walkdir(subpath, action, false, elevel);
				break;
			default:

				/*
				 * Errors are already reported directly by get_dirent_type(),
				 * and any remaining symlinks and unknown file types are
				 * ignored.
				 */
				break;
		}
	}

	FreeDir(dir);				/* we ignore any error here */

	/*
	 * It's important to fsync the destination directory itself as individual
	 * file fsyncs don't guarantee that the directory entry for the file is
	 * synced.  However, skip this if AllocateDir failed; the action function
	 * might not be robust against that.
	 */
	if (dir)
		(*action) (path, true, elevel);
}


/*
 * Hint to the OS that it should get ready to fsync() this file.
 *
 * Ignores errors trying to open unreadable files, and logs other errors at a
 * caller-specified level.
 */
#ifdef PG_FLUSH_DATA_WORKS

static void
pre_sync_fname(const char *fname, bool isdir, int elevel)
{
	int			fd;

	/* Don't try to flush directories, it'll likely just fail */
	if (isdir)
		return;

	ereport_startup_progress("syncing data directory (pre-fsync), elapsed time: %ld.%02d s, current path: %s",
							 fname);

	fd = OpenTransientFile(fname, O_RDONLY | PG_BINARY);

	if (fd < 0)
	{
		if (errno == EACCES)
			return;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fname)));
		return;
	}

	/*
	 * pg_flush_data() ignores errors, which is ok because this is only a
	 * hint.
	 */
	pg_flush_data(fd, 0, 0);

	if (CloseTransientFile(fd) != 0)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fname)));
}

#endif							/* PG_FLUSH_DATA_WORKS */

static void
datadir_fsync_fname(const char *fname, bool isdir, int elevel)
{
	ereport_startup_progress("syncing data directory (fsync), elapsed time: %ld.%02d s, current path: %s",
							 fname);

	/*
	 * We want to silently ignoring errors about unreadable files.  Pass that
	 * desire on to fsync_fname_ext().
	 */
	fsync_fname_ext(fname, isdir, true, elevel);
}

static void
unlink_if_exists_fname(const char *fname, bool isdir, int elevel)
{
	if (isdir)
	{
		if (rmdir(fname) != 0 && errno != ENOENT)
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m", fname)));
	}
	else
	{
		/* Use PathNameDeleteTemporaryFile to report filesize */
		PathNameDeleteTemporaryFile(fname, false);
	}
}
