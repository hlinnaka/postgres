/*-------------------------------------------------------------------------
 *
 * bootstrap.h
 *	  include file for the bootstrapping code
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/bootstrap/bootstrap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BOOTSTRAP_H
#define BOOTSTRAP_H

#include "catalog/pg_attribute.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"


/*
 * MAXATTR is the maximum number of attributes in a relation supported
 * at bootstrap time (i.e., the max possible in a system table).
 */
#define MAXATTR 40

#define BOOTCOL_NULL_AUTO			1
#define BOOTCOL_NULL_FORCE_NULL		2
#define BOOTCOL_NULL_FORCE_NOT_NULL 3

struct bki_parse_state
{
	MemoryContext per_line_ctx;

	Relation	boot_reldesc;
	Form_pg_attribute attrtypes[MAXATTR];
	int			numattr;

	Datum		values[MAXATTR];	/* current row's attribute values */
	bool		Nulls[MAXATTR];
	int			num_columns_read;
};
typedef struct bki_parse_state bki_parse_state;

pg_noreturn extern void BootstrapModeMain(int argc, char *argv[], bool check_only);

extern void closerel(bki_parse_state *parse_state, char *relname);
extern void boot_openrel(bki_parse_state *parse_state, char *relname);

extern void DefineAttr(bki_parse_state *parse_state, char *name, char *type, int attnum, int nullness);
extern void InsertOneTuple(bki_parse_state *parse_state);
extern void InsertOneValue(bki_parse_state *parse_state, char *value, int i);
extern void InsertOneNull(bki_parse_state *parse_state, int i);

extern void index_register(Oid heap, Oid ind, const IndexInfo *indexInfo);
extern void build_indices(bki_parse_state *parse_state);

extern void boot_get_type_io_data(Oid typid,
								  int16 *typlen,
								  bool *typbyval,
								  char *typalign,
								  char *typdelim,
								  Oid *typioparam,
								  Oid *typinput,
								  Oid *typoutput,
								  Oid *typcollation);

extern Oid	boot_get_role_oid(const char *rolname);

union YYSTYPE;
typedef void *yyscan_t;

extern int	boot_yyparse(yyscan_t yyscanner);
extern int	boot_yylex_init(yyscan_t *yyscannerp);
extern int	boot_yylex(union YYSTYPE *yylval_param, yyscan_t yyscanner);
extern struct bki_parse_state *boot_yyget_extra(yyscan_t yyscanner);
pg_noreturn extern void boot_yyerror(yyscan_t yyscanner, const char *message);

#endif							/* BOOTSTRAP_H */
