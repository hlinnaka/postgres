/*-------------------------------------------------------------------------
 *
 * postgres_ext.h
 *
 *	   This file contains declarations of things that are visible everywhere
 *	in PostgreSQL *and* are visible to clients of frontend interface libraries.
 *	For example, the Oid type is part of the API of libpq and other libraries.
 *
 *	   Declarations which are specific to a particular interface should
 *	go in the header file for that interface (such as libpq-fe.h).  This
 *	file is only for fundamental Postgres declarations.
 *
 *	   User-written C functions don't count as "external to Postgres."
 *	Those function much as local modifications to the backend itself, and
 *	use header files that are otherwise internal to Postgres to interface
 *	with the backend.
 *
 * src/include/postgres_ext.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POSTGRES_EXT_H
#define POSTGRES_EXT_H

#include "pg_config_ext.h"

/*
 * Annotations on global variables.
 *
 * PostgreSQL source code uses global variables for many different things.
 * Some global variables are nearly constants, like lookup tables initialized
 * at postmaster startup and never modified after that. Others are used to
 * hold values of GUCs. Some hold per-session state, others are reset at every
 * transaction.
 *
 * These annotations help to document how a global variable is used. These are
 * currently optional, and if the compiler doesn't support the
 * __attribute__(annotate) syntax, they are defined to nothing. In the future,
 * we could have additional assertions based on the annotations.
 *
 * When we switch to multithreaded thread-per-connection model, the per-session
 * variables are turned into thread-local variables.
 *
 * The 'pgguclifetimes' program (in src/tools/pgguclifetimes) can be used to check
 * that all global variables have been annotated.
 */
#if __has_attribute (annotate)
#define global __attribute__((annotate("global")))
#define dynamic_singleton __attribute__((annotate("dynamic_singleton")))
#define static_singleton __attribute__((annotate("static_singleton")))
#define session_local __thread __attribute__((annotate("session_local")))
#else
#define global
#define dynamic_singleton
#define static_singleton
#define session_local __thread
#endif

#define DEFINE_BOOL_GUC_ADDR(guc) \
	static bool *guc##_address(void) { return &guc; }
#define DEFINE_INT_GUC_ADDR(guc) \
	static int *guc##_address(void) { return &guc; }
#define DEFINE_REAL_GUC_ADDR(guc) \
	static float8 *guc##_address(void) { return &guc; }
#define DEFINE_STRING_GUC_ADDR(guc) \
	static char **guc##_address(void) { return &guc; }
#define DEFINE_ENUM_GUC_ADDR(guc) \
	static int *guc##_address(void) { return &guc; }

#define GUC_ADDR(guc) &guc##_address

/*
 * Object ID is a fundamental type in Postgres.
 */
typedef unsigned int Oid;

#ifdef __cplusplus
#define InvalidOid		(Oid(0))
#else
#define InvalidOid		((Oid) 0)
#endif

#define OID_MAX  UINT_MAX
/* you will need to include <limits.h> to use the above #define */

#define atooid(x) ((Oid) strtoul((x), NULL, 10))
/* the above needs <stdlib.h> */


/* Define a signed 64-bit integer type for use in client API declarations. */
typedef PG_INT64_TYPE pg_int64;

/*
 * Identifiers of error message fields.  Kept here to keep common
 * between frontend and backend, and also to export them to libpq
 * applications.
 */
#define PG_DIAG_SEVERITY		'S'
#define PG_DIAG_SEVERITY_NONLOCALIZED 'V'
#define PG_DIAG_SQLSTATE		'C'
#define PG_DIAG_MESSAGE_PRIMARY 'M'
#define PG_DIAG_MESSAGE_DETAIL	'D'
#define PG_DIAG_MESSAGE_HINT	'H'
#define PG_DIAG_STATEMENT_POSITION 'P'
#define PG_DIAG_INTERNAL_POSITION 'p'
#define PG_DIAG_INTERNAL_QUERY	'q'
#define PG_DIAG_CONTEXT			'W'
#define PG_DIAG_SCHEMA_NAME		's'
#define PG_DIAG_TABLE_NAME		't'
#define PG_DIAG_COLUMN_NAME		'c'
#define PG_DIAG_DATATYPE_NAME	'd'
#define PG_DIAG_CONSTRAINT_NAME 'n'
#define PG_DIAG_SOURCE_FILE		'F'
#define PG_DIAG_SOURCE_LINE		'L'
#define PG_DIAG_SOURCE_FUNCTION 'R'

#endif							/* POSTGRES_EXT_H */
