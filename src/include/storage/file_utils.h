/*-------------------------------------------------------------------------
 *
 * file_utils.h
 *	  Assorted utility functions to work on files
 *
 * See also common/file_utils.h for similar functions for frontend
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/file_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STORAGE_FILE_UTILS_H
#define STORAGE_FILE_UTILS_H

/* GUC parameter */
extern PGDLLIMPORT bool data_sync_retry;

/*
 * On Windows, we have to interpret EACCES as possibly meaning the same as
 * ENOENT, because if a file is unlinked-but-not-yet-gone on that platform,
 * that's what you get.  Ugh.  This code is designed so that we don't
 * actually believe these cases are okay without further evidence (namely,
 * a pending fsync request getting canceled ... see ProcessSyncRequests).
 */
#ifndef WIN32
#define FILE_POSSIBLY_DELETED(err)	((err) == ENOENT)
#else
#define FILE_POSSIBLY_DELETED(err)	((err) == ENOENT || (err) == EACCES)
#endif

extern int	pg_fsync(int fd);
extern int	pg_fsync_no_writethrough(int fd);
extern int	pg_fsync_writethrough(int fd);
extern int	pg_fdatasync(int fd);
extern bool pg_file_exists(const char *name);
extern void pg_flush_data(int fd, pgoff_t offset, pgoff_t nbytes);
extern int	pg_ftruncate(int fd, pgoff_t length);
extern int	pg_truncate(const char *path, pgoff_t length);
extern int	data_sync_elevel(int elevel);

/*
 * These functions have counterparts in common/file_utils.h for frontend code
 */
extern void fsync_fname(const char *fname, bool isdir);
extern int	fsync_fname_ext(const char *fname, bool isdir, bool ignore_perm, int elevel);
extern int	durable_rename(const char *oldfile, const char *newfile, int elevel);
extern int	durable_unlink(const char *fname, int elevel);

#endif							/* STORAGE_FILE_UTILS_H */
