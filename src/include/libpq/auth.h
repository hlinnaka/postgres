/*-------------------------------------------------------------------------
 *
 * auth.h
 *	  Definitions for network authentication routines
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/auth.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTH_H
#define AUTH_H

#include "libpq/libpq-be.h"

extern PGDLLIMPORT sighup_guc char *pg_krb_server_keyfile;
extern PGDLLIMPORT sighup_guc bool pg_krb_caseins_users;
extern PGDLLIMPORT sighup_guc bool pg_gss_accept_delegation;

extern void ClientAuthentication(Port *port);
extern void sendAuthRequest(Port *port, AuthRequest areq, const char *extradata,
							int extralen);

/* Hook for plugins to get control in ClientAuthentication() */
typedef void (*ClientAuthentication_hook_type) (Port *, int);
extern PGDLLIMPORT session_local ClientAuthentication_hook_type ClientAuthentication_hook;

/* hook type for password manglers */
typedef char *(*auth_password_hook_typ) (char *input);

/* Default LDAP password mutator hook, can be overridden by a shared library */
extern PGDLLIMPORT dynamic_singleton auth_password_hook_typ ldap_password_hook;

#endif							/* AUTH_H */
