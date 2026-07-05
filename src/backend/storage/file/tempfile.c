/*-------------------------------------------------------------------------
 *
 * tempfile.c
 *	  Temporary file management
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/tempfile.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>

#include "catalog/pg_tablespace.h"
#include "common/file_utils.h"
#include "common/pg_prng.h"
#include "storage/datadir.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/tempfile.h"
#include "utils/backend_status.h"
#include "utils/guc.h"
#include "utils/resowner.h"

/* Temporary file access initialized and not yet shut down? */
#ifdef USE_ASSERT_CHECKING
static bool temporary_files_allowed = false;
#endif

/*
 * Number of temporary files opened during the current session;
 * this is used in generation of tempfile names.
 */
static long tempFileCounter = 0;

/*
 * Array of OIDs of temp tablespaces.  (Some entries may be InvalidOid,
 * indicating that the current database's default tablespace should be used.)
 * When numTempTableSpaces is -1, this has not been set in the current
 * transaction.
 */
static Oid *tempTableSpaces = NULL;
static int	numTempTableSpaces = -1;
static int	nextTempTableSpace = 0;

static File OpenTemporaryFileInTablespace(Oid tblspcOid, bool rejectError);

static void RemovePgTempRelationFiles(const char *tsdirname);
static void RemovePgTempRelationFilesInDbspace(const char *dbspacedirname);
static void BeforeShmemExit_TempFiles(int code, Datum arg);

/*
 * InitTemporaryFileAccess --- initialize temporary file access during startup
 *
 * This is called during either normal or standalone backend start.
 * It is *not* called in the postmaster.
 *
 * This is separate from InitFileAccess() because temporary file cleanup can
 * cause pgstat reporting. As pgstat is shut down during before_shmem_exit(),
 * our reporting has to happen before that. Low level file access should be
 * available for longer, hence the separate initialization / shutdown of
 * temporary file handling.
 */
void
InitTemporaryFileAccess(void)
{
	Assert(!temporary_files_allowed);	/* call me only once */

	/*
	 * Register before-shmem-exit hook to ensure temp files are dropped while
	 * we can still report stats.
	 */
	before_shmem_exit(BeforeShmemExit_TempFiles, 0);

#ifdef USE_ASSERT_CHECKING
	temporary_files_allowed = true;
#endif
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
 * Open a temporary file that will disappear when we close it.
 *
 * This routine takes care of generating an appropriate tempfile name.
 * There's no need to pass in fileFlags or fileMode either, since only
 * one setting makes any sense for a temp file.
 *
 * Unless interXact is true, the file is remembered by CurrentResourceOwner
 * to ensure it's closed and deleted when it's no longer needed, typically at
 * the end-of-transaction. In most cases, you don't want temporary files to
 * outlive the transaction that created them, so this should be false -- but
 * if you need "somewhat" temporary storage, this might be useful. In either
 * case, the file is removed when the File is explicitly closed.
 */
File
OpenTemporaryFile(bool interXact)
{
	File		file = 0;

	Assert(temporary_files_allowed);	/* check temp file access is up */

	/*
	 * Make sure the current resource owner has space for this File before we
	 * open it, if we'll be registering it below.
	 */
	if (!interXact)
		ResourceOwnerEnlarge(CurrentResourceOwner);

	/*
	 * If some temp tablespace(s) have been given to us, try to use the next
	 * one.  If a given tablespace can't be found, we silently fall back to
	 * the database's default tablespace.
	 *
	 * BUT: if the temp file is slated to outlive the current transaction,
	 * force it into the database's default tablespace, so that it will not
	 * pose a threat to possible tablespace drop attempts.
	 */
	if (numTempTableSpaces > 0 && !interXact)
	{
		Oid			tblspcOid = GetNextTempTableSpace();

		if (OidIsValid(tblspcOid))
			file = OpenTemporaryFileInTablespace(tblspcOid, false);
	}

	/*
	 * If not, or if tablespace is bad, create in database's default
	 * tablespace.  MyDatabaseTableSpace should normally be set before we get
	 * here, but just in case it isn't, fall back to pg_default tablespace.
	 */
	if (file <= 0)
		file = OpenTemporaryFileInTablespace(MyDatabaseTableSpace ?
											 MyDatabaseTableSpace :
											 DEFAULTTABLESPACE_OID,
											 true);

	/*
	 * Register it with the current resource owner (unless it's a cross-xact
	 * temp file), and mark for deletion at close and temp_file_limit
	 * accounting.
	 */
	RegisterTemporaryFile(file, interXact ? NULL : CurrentResourceOwner,
						  FD_DELETE_AT_CLOSE | FD_TEMP_FILE_LIMIT);

	return file;
}

/*
 * Return the path of the temp directory in a given tablespace.
 */
void
TempTablespacePath(char *path, Oid tablespace)
{
	/*
	 * Identify the tempfile directory for this tablespace.
	 *
	 * If someone tries to specify pg_global, use pg_default instead.
	 */
	if (tablespace == InvalidOid ||
		tablespace == DEFAULTTABLESPACE_OID ||
		tablespace == GLOBALTABLESPACE_OID)
		snprintf(path, MAXPGPATH, "base/%s", PG_TEMP_FILES_DIR);
	else
	{
		/* All other tablespaces are accessed via symlinks */
		snprintf(path, MAXPGPATH, "%s/%u/%s/%s",
				 PG_TBLSPC_DIR, tablespace, TABLESPACE_VERSION_DIRECTORY,
				 PG_TEMP_FILES_DIR);
	}
}

/*
 * Open a temporary file in a specific tablespace.
 * Subroutine for OpenTemporaryFile, which see for details.
 */
static File
OpenTemporaryFileInTablespace(Oid tblspcOid, bool rejectError)
{
	char		tempdirpath[MAXPGPATH];
	char		tempfilepath[MAXPGPATH];
	File		file;

	TempTablespacePath(tempdirpath, tblspcOid);

	/*
	 * Generate a tempfile name that should be unique within the current
	 * database instance.
	 */
	snprintf(tempfilepath, sizeof(tempfilepath), "%s/%s%d.%ld",
			 tempdirpath, PG_TEMP_FILE_PREFIX, MyProcPid, tempFileCounter++);

	/*
	 * Open the file.  Note: we don't use O_EXCL, in case there is an orphaned
	 * temp file that can be reused.
	 */
	file = PathNameOpenFile(tempfilepath,
							O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (file <= 0)
	{
		/*
		 * We might need to create the tablespace's tempfile directory, if no
		 * one has yet done so.
		 *
		 * Don't check for an error from MakePGDirectory; it could fail if
		 * someone else just did the same thing.  If it doesn't work then
		 * we'll bomb out on the second create attempt, instead.
		 */
		(void) MakePGDirectory(tempdirpath);

		file = PathNameOpenFile(tempfilepath,
								O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
		if (file <= 0 && rejectError)
			elog(ERROR, "could not create temporary file \"%s\": %m",
				 tempfilepath);
	}

	return file;
}


/*
 * Create a new file.  The directory containing it must already exist.  Files
 * created this way are subject to temp_file_limit and are automatically
 * closed at end of transaction, but are not automatically deleted on close
 * because they are intended to be shared between cooperating backends.
 *
 * If the file is inside the top-level temporary directory, its name should
 * begin with PG_TEMP_FILE_PREFIX so that it can be identified as temporary
 * and deleted at startup by RemovePgTempFiles().  Alternatively, it can be
 * inside a directory created with PathNameCreateTemporaryDir(), in which case
 * the prefix isn't needed.
 */
File
PathNameCreateTemporaryFile(const char *path, bool error_on_failure)
{
	File		file;

	Assert(temporary_files_allowed);	/* check temp file access is up */

	ResourceOwnerEnlarge(CurrentResourceOwner);

	/*
	 * Open the file.  Note: we don't use O_EXCL, in case there is an orphaned
	 * temp file that can be reused.
	 */
	file = PathNameOpenFile(path, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (file <= 0)
	{
		if (error_on_failure)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create temporary file \"%s\": %m",
							path)));
		else
			return file;
	}

	/* Register it for automatic close and temp_file_limit accounting */
	RegisterTemporaryFile(file, CurrentResourceOwner, FD_TEMP_FILE_LIMIT);

	return file;
}

/*
 * Open a file that was created with PathNameCreateTemporaryFile, possibly in
 * another backend.  Files opened this way don't count against the
 * temp_file_limit of the caller, are automatically closed at the end of the
 * transaction but are not deleted on close.
 */
File
PathNameOpenTemporaryFile(const char *path, int mode)
{
	File		file;

	Assert(temporary_files_allowed);	/* check temp file access is up */

	ResourceOwnerEnlarge(CurrentResourceOwner);

	file = PathNameOpenFile(path, mode | PG_BINARY);

	/* If no such file, then we don't raise an error. */
	if (file <= 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open temporary file \"%s\": %m",
						path)));

	if (file > 0)
	{
		/*
		 * Register it for automatic close (but no delete-at-close or
		 * temp_file_limit accounting)
		 */
		RegisterTemporaryFile(file, CurrentResourceOwner, 0);
	}

	return file;
}

/*
 * Delete a file by pathname.  Return true if the file existed, false if
 * didn't.
 */
bool
PathNameDeleteTemporaryFile(const char *path, bool error_on_failure)
{
	struct stat filestats;
	int			stat_errno;

	/* Get the final size for pgstat reporting. */
	if (stat(path, &filestats) != 0)
		stat_errno = errno;
	else
		stat_errno = 0;

	/*
	 * Unlike FileClose's automatic file deletion code, we tolerate
	 * non-existence to support BufFileDeleteFileSet which doesn't know how
	 * many segments it has to delete until it runs out.
	 */
	if (stat_errno == ENOENT)
		return false;

	if (unlink(path) < 0)
	{
		if (errno != ENOENT)
			ereport(error_on_failure ? ERROR : LOG,
					(errcode_for_file_access(),
					 errmsg("could not unlink temporary file \"%s\": %m",
							path)));
		return false;
	}

	if (stat_errno == 0)
		ReportTemporaryFileUsage(path, filestats.st_size);
	else
	{
		errno = stat_errno;
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	}

	return true;
}


/*
 * SetTempTablespaces
 *
 * Define a list (actually an array) of OIDs of tablespaces to use for
 * temporary files.  This list will be used until end of transaction,
 * unless this function is called again before then.  It is caller's
 * responsibility that the passed-in array has adequate lifespan (typically
 * it'd be allocated in TopTransactionContext).
 *
 * Some entries of the array may be InvalidOid, indicating that the current
 * database's default tablespace should be used.
 */
void
SetTempTablespaces(Oid *tableSpaces, int numSpaces)
{
	Assert(numSpaces >= 0);
	tempTableSpaces = tableSpaces;
	numTempTableSpaces = numSpaces;

	/*
	 * Select a random starting point in the list.  This is to minimize
	 * conflicts between backends that are most likely sharing the same list
	 * of temp tablespaces.  Note that if we create multiple temp files in the
	 * same transaction, we'll advance circularly through the list --- this
	 * ensures that large temporary sort files are nicely spread across all
	 * available tablespaces.
	 */
	if (numSpaces > 1)
		nextTempTableSpace = pg_prng_uint64_range(&pg_global_prng_state,
												  0, numSpaces - 1);
	else
		nextTempTableSpace = 0;
}

/*
 * TempTablespacesAreSet
 *
 * Returns true if SetTempTablespaces has been called in current transaction.
 * (This is just so that tablespaces.c doesn't need its own per-transaction
 * state.)
 */
bool
TempTablespacesAreSet(void)
{
	return (numTempTableSpaces >= 0);
}

/*
 * GetTempTablespaces
 *
 * Populate an array with the OIDs of the tablespaces that should be used for
 * temporary files.  (Some entries may be InvalidOid, indicating that the
 * current database's default tablespace should be used.)  At most numSpaces
 * entries will be filled.
 * Returns the number of OIDs that were copied into the output array.
 */
int
GetTempTablespaces(Oid *tableSpaces, int numSpaces)
{
	int			i;

	Assert(TempTablespacesAreSet());
	for (i = 0; i < numTempTableSpaces && i < numSpaces; ++i)
		tableSpaces[i] = tempTableSpaces[i];

	return i;
}

/*
 * GetNextTempTableSpace
 *
 * Select the next temp tablespace to use.  A result of InvalidOid means
 * to use the current database's default tablespace.
 */
Oid
GetNextTempTableSpace(void)
{
	if (numTempTableSpaces > 0)
	{
		/* Advance nextTempTableSpace counter with wraparound */
		if (++nextTempTableSpace >= numTempTableSpaces)
			nextTempTableSpace = 0;
		return tempTableSpaces[nextTempTableSpace];
	}
	return InvalidOid;
}

/*
 * Remove temporary and temporary relation files left over from a prior
 * postmaster session
 *
 * This should be called during postmaster startup.  It will forcibly
 * remove any leftover files created by OpenTemporaryFile and any leftover
 * temporary relation files created by mdcreate.
 *
 * During post-backend-crash restart cycle, this routine is called when
 * remove_temp_files_after_crash GUC is enabled. Multiple crashes while
 * queries are using temp files could result in useless storage usage that can
 * only be reclaimed by a service restart. The argument against enabling it is
 * that someone might want to examine the temporary files for debugging
 * purposes. This does however mean that OpenTemporaryFile had better allow for
 * collision with an existing temp file name.
 *
 * NOTE: this function and its subroutines generally report syscall failures
 * with ereport(LOG) and keep going.  Removing temp files is not so critical
 * that we should fail to start the database when we can't do it.
 */
void
RemovePgTempFiles(void)
{
	char		temp_path[MAXPGPATH + sizeof(PG_TBLSPC_DIR) + sizeof(TABLESPACE_VERSION_DIRECTORY) + sizeof(PG_TEMP_FILES_DIR)];
	DIR		   *spc_dir;
	struct dirent *spc_de;

	/*
	 * First process temp files in pg_default ($PGDATA/base)
	 */
	snprintf(temp_path, sizeof(temp_path), "base/%s", PG_TEMP_FILES_DIR);
	RemovePgTempFilesInDir(temp_path, true, false);
	RemovePgTempRelationFiles("base");

	/*
	 * Cycle through temp directories for all non-default tablespaces.
	 */
	spc_dir = AllocateDir(PG_TBLSPC_DIR);

	while ((spc_de = ReadDirExtended(spc_dir, PG_TBLSPC_DIR, LOG)) != NULL)
	{
		if (strcmp(spc_de->d_name, ".") == 0 ||
			strcmp(spc_de->d_name, "..") == 0)
			continue;

		snprintf(temp_path, sizeof(temp_path), "%s/%s/%s/%s",
				 PG_TBLSPC_DIR, spc_de->d_name, TABLESPACE_VERSION_DIRECTORY,
				 PG_TEMP_FILES_DIR);
		RemovePgTempFilesInDir(temp_path, true, false);

		snprintf(temp_path, sizeof(temp_path), "%s/%s/%s",
				 PG_TBLSPC_DIR, spc_de->d_name, TABLESPACE_VERSION_DIRECTORY);
		RemovePgTempRelationFiles(temp_path);
	}

	FreeDir(spc_dir);

	/*
	 * In EXEC_BACKEND case there is a pgsql_tmp directory at the top level of
	 * DataDir as well.  However, that is *not* cleaned here because doing so
	 * would create a race condition.  It's done separately, earlier in
	 * postmaster startup.
	 */
}

/*
 * Process one pgsql_tmp directory for RemovePgTempFiles.
 *
 * If missing_ok is true, it's all right for the named directory to not exist.
 * Any other problem results in a LOG message.  (missing_ok should be true at
 * the top level, since pgsql_tmp directories are not created until needed.)
 *
 * At the top level, this should be called with unlink_all = false, so that
 * only files matching the temporary name prefix will be unlinked.  When
 * recursing it will be called with unlink_all = true to unlink everything
 * under a top-level temporary directory.
 *
 * (These two flags could be replaced by one, but it seems clearer to keep
 * them separate.)
 */
void
RemovePgTempFilesInDir(const char *tmpdirname, bool missing_ok, bool unlink_all)
{
	DIR		   *temp_dir;
	struct dirent *temp_de;
	char		rm_path[MAXPGPATH * 2];

	temp_dir = AllocateDir(tmpdirname);

	if (temp_dir == NULL && errno == ENOENT && missing_ok)
		return;

	while ((temp_de = ReadDirExtended(temp_dir, tmpdirname, LOG)) != NULL)
	{
		if (strcmp(temp_de->d_name, ".") == 0 ||
			strcmp(temp_de->d_name, "..") == 0)
			continue;

		snprintf(rm_path, sizeof(rm_path), "%s/%s",
				 tmpdirname, temp_de->d_name);

		if (unlink_all ||
			strncmp(temp_de->d_name,
					PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) == 0)
		{
			PGFileType	type = get_dirent_type(rm_path, temp_de, false, LOG);

			if (type == PGFILETYPE_ERROR)
				continue;
			else if (type == PGFILETYPE_DIR)
			{
				/* recursively remove contents, then directory itself */
				RemovePgTempFilesInDir(rm_path, false, true);

				if (rmdir(rm_path) < 0)
					ereport(LOG,
							(errcode_for_file_access(),
							 errmsg("could not remove directory \"%s\": %m",
									rm_path)));
			}
			else
			{
				if (unlink(rm_path) < 0)
					ereport(LOG,
							(errcode_for_file_access(),
							 errmsg("could not remove file \"%s\": %m",
									rm_path)));
			}
		}
		else
			ereport(LOG,
					(errmsg("unexpected file found in temporary-files directory: \"%s\"",
							rm_path)));
	}

	FreeDir(temp_dir);
}

/* Process one tablespace directory, look for per-DB subdirectories */
static void
RemovePgTempRelationFiles(const char *tsdirname)
{
	DIR		   *ts_dir;
	struct dirent *de;
	char		dbspace_path[MAXPGPATH * 2];

	ts_dir = AllocateDir(tsdirname);

	while ((de = ReadDirExtended(ts_dir, tsdirname, LOG)) != NULL)
	{
		/*
		 * We're only interested in the per-database directories, which have
		 * numeric names.  Note that this code will also (properly) ignore "."
		 * and "..".
		 */
		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;

		snprintf(dbspace_path, sizeof(dbspace_path), "%s/%s",
				 tsdirname, de->d_name);
		RemovePgTempRelationFilesInDbspace(dbspace_path);
	}

	FreeDir(ts_dir);
}

/* Process one per-dbspace directory for RemovePgTempRelationFiles */
static void
RemovePgTempRelationFilesInDbspace(const char *dbspacedirname)
{
	DIR		   *dbspace_dir;
	struct dirent *de;
	char		rm_path[MAXPGPATH * 2];

	dbspace_dir = AllocateDir(dbspacedirname);

	while ((de = ReadDirExtended(dbspace_dir, dbspacedirname, LOG)) != NULL)
	{
		if (!looks_like_temp_rel_name(de->d_name))
			continue;

		snprintf(rm_path, sizeof(rm_path), "%s/%s",
				 dbspacedirname, de->d_name);

		if (unlink(rm_path) < 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							rm_path)));
	}

	FreeDir(dbspace_dir);
}

/* t<digits>_<digits>, or t<digits>_<digits>_<forkname> */
bool
looks_like_temp_rel_name(const char *name)
{
	int			pos;
	int			savepos;

	/* Must start with "t". */
	if (name[0] != 't')
		return false;

	/* Followed by a non-empty string of digits and then an underscore. */
	for (pos = 1; isdigit((unsigned char) name[pos]); ++pos)
		;
	if (pos == 1 || name[pos] != '_')
		return false;

	/* Followed by another nonempty string of digits. */
	for (savepos = ++pos; isdigit((unsigned char) name[pos]); ++pos)
		;
	if (savepos == pos)
		return false;

	/* We might have _forkname or .segment or both. */
	if (name[pos] == '_')
	{
		int			forkchar = forkname_chars(&name[pos + 1], NULL);

		if (forkchar <= 0)
			return false;
		pos += forkchar + 1;
	}
	if (name[pos] == '.')
	{
		int			segchar;

		for (segchar = 1; isdigit((unsigned char) name[pos + segchar]); ++segchar)
			;
		if (segchar <= 1)
			return false;
		pos += segchar;
	}

	/* Now we should be at the end. */
	if (name[pos] != '\0')
		return false;
	return true;
}


/*
 * AtEOXact_TempFiles
 *
 * This routine is called during transaction commit or abort.  We forget any
 * transaction-local temp tablespace list.
 */
void
AtEOXact_TempFiles(void)
{
	tempTableSpaces = NULL;
	numTempTableSpaces = -1;
}

/*
 * BeforeShmemExit_TempFiles
 *
 * before_shmem_exit hook to clean up temp files during backend shutdown.
 * Here, we want to clean up *all* temp files including interXact ones.
 */
static void
BeforeShmemExit_TempFiles(int code, Datum arg)
{
	CleanupTempFiles();

	/* prevent further temp files from being created */
#ifdef USE_ASSERT_CHECKING
	temporary_files_allowed = false;
#endif
}
