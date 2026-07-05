/*-------------------------------------------------------------------------
 *
 * datadir.h
 *	  Functions to work on the datadir
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/datadir.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATADIR_H
#define DATADIR_H

/* GUC parameter */
extern PGDLLIMPORT int recovery_init_sync_method;

/* Make a directory with default permissions */
extern int	MakePGDirectory(const char *directoryName);

extern void SyncDataDirectory(void);

extern void PathNameCreateTemporaryDir(const char *basedir, const char *directory);
extern void PathNameDeleteTemporaryDir(const char *dirname);

#endif							/* DATADIR_H */
