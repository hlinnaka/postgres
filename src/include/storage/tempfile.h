/*-------------------------------------------------------------------------
 *
 * tempfile.h
 *	  Temporary file management
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/tempfile.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TEMPFILE_H
#define TEMPFILE_H

#include "storage/fd.h"

extern File OpenTemporaryFile(bool interXact);

/* Operations used for sharing named temporary files */
extern File PathNameCreateTemporaryFile(const char *path, bool error_on_failure);
extern File PathNameOpenTemporaryFile(const char *path, int mode);
extern bool PathNameDeleteTemporaryFile(const char *path, bool error_on_failure);
extern void TempTablespacePath(char *path, Oid tablespace);

/* Miscellaneous support routines */
extern void InitTemporaryFileAccess(void);
extern void SetTempTablespaces(Oid *tableSpaces, int numSpaces);
extern bool TempTablespacesAreSet(void);
extern int	GetTempTablespaces(Oid *tableSpaces, int numSpaces);
extern Oid	GetNextTempTableSpace(void);
extern void RemovePgTempFiles(void);
extern void RemovePgTempFilesInDir(const char *tmpdirname, bool missing_ok,
								   bool unlink_all);

extern bool looks_like_temp_rel_name(const char *name);

extern void AtEOXact_TempFiles(void);

#endif							/* TEMPFILE_H */
