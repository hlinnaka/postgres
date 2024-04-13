/*-------------------------------------------------------------------------
 *
 * bootstrap.c
 *	  routines to support running postgres in 'bootstrap' mode
 *	bootstrap mode is used to create the initial template database
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/bootstrap/bootstrap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <signal.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/xact.h"
#include "bootstrap/bootstrap.h"
#include "catalog/index.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "common/link-canary.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "port/pg_getopt_ctx.h"
#include "postmaster/postmaster.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/shmem_internal.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relmapper.h"


static void CheckerModeMain(void);
static void bootstrap_signals(void);
static Form_pg_attribute AllocateAttribute(void);
static void InsertOneProargdefaultsValue(bki_parse_state *parse_state, char *value);
static void populate_typ_list(void);
static const struct typinfo *get_builtin_type(char *type);
static struct typinfo *get_type(char *type);
static void cleanup(bki_parse_state *parse_state);


/*
 * Basic information associated with each type.  This is used before
 * pg_type is filled, so it has to cover the datatypes used as column types
 * in the core "bootstrapped" catalogs.
 *
 *		XXX several of these input/output functions do catalog scans
 *			(e.g., F_REGPROCIN scans pg_proc).  this obviously creates some
 *			order dependencies in the catalog creation process.
 */
struct typinfo
{
	char		typname[NAMEDATALEN];
	Oid			oid;
	Oid			typelem;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typstorage;
	Oid			typcollation;
	Oid			typinput;
	Oid			typoutput;

	/* XXX: missing on purpose */
	char		typdelim;
};

static const struct typinfo TypInfo[] = {
	{"bool", BOOLOID, 0, 1, true, TYPALIGN_CHAR, TYPSTORAGE_PLAIN, InvalidOid,
	F_BOOLIN, F_BOOLOUT},
	{"bytea", BYTEAOID, 0, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_BYTEAIN, F_BYTEAOUT},
	{"char", CHAROID, 0, 1, true, TYPALIGN_CHAR, TYPSTORAGE_PLAIN, InvalidOid,
	F_CHARIN, F_CHAROUT},
	{"cstring", CSTRINGOID, 0, -2, false, TYPALIGN_CHAR, TYPSTORAGE_PLAIN, InvalidOid,
	F_CSTRING_IN, F_CSTRING_OUT},
	{"int2", INT2OID, 0, 2, true, TYPALIGN_SHORT, TYPSTORAGE_PLAIN, InvalidOid,
	F_INT2IN, F_INT2OUT},
	{"int4", INT4OID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_INT4IN, F_INT4OUT},
	{"int8", INT8OID, 0, 8, true, TYPALIGN_DOUBLE, TYPSTORAGE_PLAIN, InvalidOid,
	F_INT8IN, F_INT8OUT},
	{"float4", FLOAT4OID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_FLOAT4IN, F_FLOAT4OUT},
	{"float8", FLOAT8OID, 0, 8, true, TYPALIGN_DOUBLE, TYPSTORAGE_PLAIN, InvalidOid,
	F_FLOAT8IN, F_FLOAT8OUT},
	{"name", NAMEOID, CHAROID, NAMEDATALEN, false, TYPALIGN_CHAR, TYPSTORAGE_PLAIN, C_COLLATION_OID,
	F_NAMEIN, F_NAMEOUT},
	{"regproc", REGPROCOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_REGPROCIN, F_REGPROCOUT},
	{"text", TEXTOID, 0, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, DEFAULT_COLLATION_OID,
	F_TEXTIN, F_TEXTOUT},
	{"jsonb", JSONBOID, 0, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_JSONB_IN, F_JSONB_OUT},
	{"oid", OIDOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_OIDIN, F_OIDOUT},
	{"aclitem", ACLITEMOID, 0, 16, false, TYPALIGN_DOUBLE, TYPSTORAGE_PLAIN, InvalidOid,
	F_ACLITEMIN, F_ACLITEMOUT},
	{"pg_node_tree", PG_NODE_TREEOID, 0, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, DEFAULT_COLLATION_OID,
	F_PG_NODE_TREE_IN, F_PG_NODE_TREE_OUT},
	{"int2vector", INT2VECTOROID, INT2OID, -1, false, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_INT2VECTORIN, F_INT2VECTOROUT},
	{"oidvector", OIDVECTOROID, OIDOID, -1, false, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_OIDVECTORIN, F_OIDVECTOROUT},
	{"_int4", INT4ARRAYOID, INT4OID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_text", TEXTARRAYOID, TEXTOID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, DEFAULT_COLLATION_OID,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_oid", OIDARRAYOID, OIDOID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_char", CHARARRAYOID, CHAROID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_aclitem", ACLITEMARRAYOID, ACLITEMOID, -1, false, TYPALIGN_DOUBLE, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT}
};

static const int n_types = sizeof(TypInfo) / sizeof(struct typinfo);

static pg_global List *Typ = NIL;			/* List of struct typinfo* */

/*
 * Basic information about built-in roles.
 *
 * Presently, this need only list roles that are mentioned in aclitem arrays
 * in the catalog .dat files.  We might as well list everything that is in
 * pg_authid.dat, since there aren't that many.  Like pg_authid.dat, we
 * represent the bootstrap superuser's name as "POSTGRES", even though it
 * (probably) won't be that in the finished installation; this means aclitem
 * entries in .dat files must spell it like that.
 */
struct rolinfo
{
	const char *rolname;
	Oid			oid;
};

static const struct rolinfo RolInfo[] = {
	{"POSTGRES", BOOTSTRAP_SUPERUSERID},
	{"pg_database_owner", ROLE_PG_DATABASE_OWNER},
	{"pg_read_all_data", ROLE_PG_READ_ALL_DATA},
	{"pg_write_all_data", ROLE_PG_WRITE_ALL_DATA},
	{"pg_monitor", ROLE_PG_MONITOR},
	{"pg_read_all_settings", ROLE_PG_READ_ALL_SETTINGS},
	{"pg_read_all_stats", ROLE_PG_READ_ALL_STATS},
	{"pg_stat_scan_tables", ROLE_PG_STAT_SCAN_TABLES},
	{"pg_read_server_files", ROLE_PG_READ_SERVER_FILES},
	{"pg_write_server_files", ROLE_PG_WRITE_SERVER_FILES},
	{"pg_execute_server_program", ROLE_PG_EXECUTE_SERVER_PROGRAM},
	{"pg_signal_backend", ROLE_PG_SIGNAL_BACKEND},
	{"pg_checkpoint", ROLE_PG_CHECKPOINT},
	{"pg_maintain", ROLE_PG_MAINTAIN},
	{"pg_use_reserved_connections", ROLE_PG_USE_RESERVED_CONNECTIONS},
	{"pg_create_subscription", ROLE_PG_CREATE_SUBSCRIPTION},
	{"pg_signal_autovacuum_worker", ROLE_PG_SIGNAL_AUTOVACUUM_WORKER}
};


static pg_global MemoryContext nogc = NULL;	/* special no-gc mem context */

/*
 *	At bootstrap time, we first declare all the indices to be built, and
 *	then build them.  The IndexList structure stores enough information
 *	to allow us to build the indices after they've been declared.
 */

typedef struct _IndexList
{
	Oid			il_heap;
	Oid			il_ind;
	IndexInfo  *il_info;
	struct _IndexList *il_next;
} IndexList;

static pg_global IndexList *ILHead = NULL;


/*
 * In shared memory checker mode, all we really want to do is create shared
 * memory and semaphores (just to prove we can do it with the current GUC
 * settings).  Since, in fact, that was already done by
 * CreateSharedMemoryAndSemaphores(), we have nothing more to do here.
 */
static void
CheckerModeMain(void)
{
	proc_exit(0);
}

/*
 *	 The main entry point for running the backend in bootstrap mode
 *
 *	 The bootstrap mode is used to initialize the template database.
 *	 The bootstrap backend doesn't speak SQL, but instead expects
 *	 commands in a special bootstrap language.
 *
 *	 When check_only is true, startup is done only far enough to verify that
 *	 the current configuration, particularly the passed in options pertaining
 *	 to shared memory sizing, options work (or at least do not cause an error
 *	 up to shared memory creation).
 */
void
BootstrapModeMain(int argc, char *argv[], bool check_only)
{
	int			i;
	char	   *progname = argv[0];
	pg_getopt_ctx optctx;
	int			flag;
	char	   *userDoption = NULL;
	uint32		bootstrap_data_checksum_version = PG_DATA_CHECKSUM_OFF;
	yyscan_t	scanner;
	bki_parse_state parse_state = { NULL };

	Assert(!IsUnderPostmaster);

	InitStandaloneProcess(argv[0]);

	/* Set defaults, to be overridden by explicit options below */
	InitializeGUCOptions(false);

	/* an initial --boot or --check should be present */
	Assert(argc > 1
		   && (strcmp(argv[1], "--boot") == 0
			   || strcmp(argv[1], "--check") == 0));
	argv++;
	argc--;

	pg_getopt_start(&optctx, argc, argv, "B:c:d:D:Fkr:X:-:");
	while ((flag = pg_getopt_next(&optctx)) != -1)
	{
		switch (flag)
		{
			case 'B':
				SetConfigOption("shared_buffers", optctx.optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case '-':

				/*
				 * Error if the user misplaced a special must-be-first option
				 * for dispatching to a subprogram.  parse_dispatch_option()
				 * returns DISPATCH_POSTMASTER if it doesn't find a match, so
				 * error for anything else.
				 */
				if (parse_dispatch_option(optctx.optarg) != DISPATCH_POSTMASTER)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("--%s must be first argument", optctx.optarg)));

				pg_fallthrough;
			case 'c':
				{
					char	   *name,
							   *value;

					ParseLongOption(optctx.optarg, &name, &value);
					if (!value)
					{
						if (flag == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optctx.optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optctx.optarg)));
					}

					SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
					pfree(name);
					pfree(value);
					break;
				}
			case 'D':
				userDoption = pstrdup(optctx.optarg);
				break;
			case 'd':
				{
					/* Turn on debugging for the bootstrap process. */
					char	   *debugstr;

					debugstr = psprintf("debug%s", optctx.optarg);
					SetConfigOption("log_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					SetConfigOption("client_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					pfree(debugstr);
				}
				break;
			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'k':
				bootstrap_data_checksum_version = PG_DATA_CHECKSUM_VERSION;
				break;
			case 'r':
				strlcpy(OutputFileName, optctx.optarg, MAXPGPATH);
				break;
			case 'X':
				SetConfigOption("wal_segment_size", optctx.optarg, PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
				break;
			default:
				write_stderr("Try \"%s --help\" for more information.\n",
							 progname);
				proc_exit(1);
				break;
		}
	}

	if (argc != optctx.optind)
	{
		write_stderr("%s: invalid command-line arguments\n", progname);
		proc_exit(1);
	}

	/* Acquire configuration parameters */
	if (!SelectConfigFiles(userDoption, progname))
		proc_exit(1);

	/*
	 * Validate we have been given a reasonable-looking DataDir and change
	 * into it
	 */
	checkDataDir();
	ChangeToDataDir();

	CreateDataDirLockFile(false);

	SetProcessingMode(BootstrapProcessing);
	IgnoreSystemIndexes = true;

	RegisterBuiltinShmemCallbacks();

	InitializeMaxBackends();

	/*
	 * Even though bootstrapping runs in single-process mode, initialize
	 * postmaster child slots array so that --check can detect running out of
	 * shared memory or other resources if max_connections is set too high.
	 */
	InitPostmasterChildSlots();

	InitializeFastPathLocks();

	ShmemCallRequestCallbacks();
	CreateSharedMemoryAndSemaphores();

	/*
	 * Estimate number of openable files.  This is essential too in --check
	 * mode, because on some platforms semaphores count as open files.
	 */
	set_max_safe_fds();

	/*
	 * XXX: It might make sense to move this into its own function at some
	 * point. Right now it seems like it'd cause more code duplication than
	 * it's worth.
	 */
	if (check_only)
	{
		SetProcessingMode(NormalProcessing);
		CheckerModeMain();
		abort();
	}

	/*
	 * Do backend-like initialization for bootstrap mode
	 */
	InitProcess();

	BaseInit();

	bootstrap_signals();
	BootStrapXLOG(bootstrap_data_checksum_version);

	/*
	 * To ensure that src/common/link-canary.c is linked into the backend, we
	 * must call it from somewhere.  Here is as good as anywhere.
	 */
	if (pg_link_canary_is_frontend())
		elog(ERROR, "backend is incorrectly linked to frontend functions");

	InitPostgres(NULL, InvalidOid, NULL, InvalidOid, 0, NULL);

	/* Initialize stuff for bootstrap-file processing */
	for (i = 0; i < MAXATTR; i++)
	{
		parse_state.attrtypes[i] = NULL;
		parse_state.Nulls[i] = false;
	}

	if (boot_yylex_init(&scanner) != 0)
		elog(ERROR, "yylex_init() failed: %m");

	/*
	 * Process bootstrap input.
	 */
	StartTransactionCommand();
	boot_yyparse(scanner);
	CommitTransactionCommand();

	/*
	 * We should now know about all mapped relations, so it's okay to write
	 * out the initial relation mapping files.
	 */
	RelationMapFinishBootstrap();

	/* Clean up and exit */
	cleanup(&parse_state);
	proc_exit(0);
}


/* ----------------------------------------------------------------
 *						misc functions
 * ----------------------------------------------------------------
 */

/*
 * Set up signal handling for a bootstrap process
 */
static void
bootstrap_signals(void)
{
	Assert(!IsUnderPostmaster);

	/*
	 * We don't actually need any non-default signal handling in bootstrap
	 * mode; "curl up and die" is a sufficient response for all these cases.
	 * Let's set that handling explicitly, as documentation if nothing else.
	 */
	pqsignal(SIGHUP, PG_SIG_DFL);
	pqsignal(SIGINT, PG_SIG_DFL);
	pqsignal(SIGTERM, PG_SIG_DFL);
	pqsignal(SIGQUIT, PG_SIG_DFL);
}

/* ----------------------------------------------------------------
 *				MANUAL BACKEND INTERACTIVE INTERFACE COMMANDS
 * ----------------------------------------------------------------
 */

/* ----------------
 *		boot_openrel
 *
 * Execute BKI OPEN command.
 * ----------------
 */
void
boot_openrel(bki_parse_state *parse_state, char *relname)
{
	int			i;

	elog(LOG, "OPEN %s", relname);

	if (strlen(relname) >= NAMEDATALEN)
		relname[NAMEDATALEN - 1] = '\0';

	/*
	 * pg_type must be filled before any OPEN command is executed, hence we
	 * can now populate Typ if we haven't yet.
	 */
	if (Typ == NIL)
		populate_typ_list();

	if (parse_state->boot_reldesc != NULL)
		closerel(parse_state, NULL);

	elog(DEBUG4, "open relation %s, attrsize %d",
		 relname, (int) ATTRIBUTE_FIXED_PART_SIZE);

	parse_state->boot_reldesc = table_openrv(makeRangeVar(NULL, relname, -1), NoLock);
	parse_state->numattr = RelationGetNumberOfAttributes(parse_state->boot_reldesc);
	for (i = 0; i < parse_state->numattr; i++)
	{
		if (parse_state->attrtypes[i] == NULL)
			parse_state->attrtypes[i] = AllocateAttribute();
		memmove(parse_state->attrtypes[i],
				TupleDescAttr(parse_state->boot_reldesc->rd_att, i),
				ATTRIBUTE_FIXED_PART_SIZE);

		{
			Form_pg_attribute at = parse_state->attrtypes[i];

			elog(DEBUG4, "create attribute %d name %s len %d num %d type %u",
				 i, NameStr(at->attname), at->attlen, at->attnum,
				 at->atttypid);
		}
	}
}

/* ----------------
 *		closerel
 * ----------------
 */
void
closerel(bki_parse_state *parse_state, char *relname)
{
	if (relname)
	{
		if (parse_state->boot_reldesc)
		{
			if (strcmp(RelationGetRelationName(parse_state->boot_reldesc), relname) != 0)
				elog(ERROR, "close of %s when %s was expected",
					 relname, RelationGetRelationName(parse_state->boot_reldesc));
		}
		else
			elog(ERROR, "close of %s before any relation was opened",
				 relname);
	}

	if (parse_state->boot_reldesc == NULL)
		elog(ERROR, "no open relation to close");
	else
	{
		elog(DEBUG4, "close relation %s",
			 RelationGetRelationName(parse_state->boot_reldesc));
		table_close(parse_state->boot_reldesc, NoLock);
		parse_state->boot_reldesc = NULL;
	}
}



/* ----------------
 * DEFINEATTR()
 *
 * define a <field,type> pair
 * if there are n fields in a relation to be created, this routine
 * will be called n times
 * ----------------
 */
void
DefineAttr(bki_parse_state *parse_state, char *name, char *type, int attnum, int nullness)
{
	Form_pg_attribute *attrtypes = parse_state->attrtypes;
	const struct typinfo *tp = NULL;

	if (parse_state->boot_reldesc != NULL)
	{
		elog(WARNING, "no open relations allowed with CREATE command");
		closerel(parse_state, NULL);
	}

	if (attrtypes[attnum] == NULL)
		attrtypes[attnum] = AllocateAttribute();
	MemSet(parse_state->attrtypes[attnum], 0, ATTRIBUTE_FIXED_PART_SIZE);

	namestrcpy(&attrtypes[attnum]->attname, name);
	elog(DEBUG4, "column %s %s", NameStr(attrtypes[attnum]->attname), type);
	attrtypes[attnum]->attnum = attnum + 1;

	if (Typ == NIL)
		tp = get_builtin_type(type);
	else
		tp = get_type(type);
	if (!tp)
	{
		/*
		 * The type wasn't known; reload the pg_type contents and check again
		 * to handle composite types, added since last populating the list.
		 */
		list_free_deep(Typ);
		Typ = NIL;
		populate_typ_list();

		tp = get_type(type);
		if (!tp)
			elog(ERROR, "unrecognized type \"%s\"", type);
	}

	attrtypes[attnum]->atttypid = tp->oid;
	attrtypes[attnum]->attlen = tp->typlen;
	attrtypes[attnum]->attbyval = tp->typbyval;
	attrtypes[attnum]->attalign = tp->typalign;
	attrtypes[attnum]->attstorage = tp->typstorage;
	attrtypes[attnum]->attcompression = InvalidCompressionMethod;
	attrtypes[attnum]->attcollation = tp->typcollation;
	/* if an array type, assume 1-dimensional attribute */
	if (tp->typelem != InvalidOid && tp->typlen < 0)
		attrtypes[attnum]->attndims = 1;
	else
		attrtypes[attnum]->attndims = 0;

	/*
	 * If a system catalog column is collation-aware, force it to use C
	 * collation, so that its behavior is independent of the database's
	 * collation.  This is essential to allow template0 to be cloned with a
	 * different database collation.
	 */
	if (OidIsValid(attrtypes[attnum]->attcollation))
		attrtypes[attnum]->attcollation = C_COLLATION_OID;

	attrtypes[attnum]->atttypmod = -1;
	attrtypes[attnum]->attislocal = true;

	if (nullness == BOOTCOL_NULL_FORCE_NOT_NULL)
	{
		attrtypes[attnum]->attnotnull = true;
	}
	else if (nullness == BOOTCOL_NULL_FORCE_NULL)
	{
		attrtypes[attnum]->attnotnull = false;
	}
	else
	{
		Assert(nullness == BOOTCOL_NULL_AUTO);

		/*
		 * Mark as "not null" if type is fixed-width and prior columns are
		 * likewise fixed-width and not-null.  This corresponds to case where
		 * column can be accessed directly via C struct declaration.
		 */
		if (attrtypes[attnum]->attlen > 0)
		{
			int			i;

			/* check earlier attributes */
			for (i = 0; i < attnum; i++)
			{
				if (attrtypes[i]->attlen <= 0 ||
					!attrtypes[i]->attnotnull)
					break;
			}
			if (i == attnum)
				attrtypes[attnum]->attnotnull = true;
		}
	}
}


/* ----------------
 *		InsertOneTuple
 *
 * If objectid is not zero, it is a specific OID to assign to the tuple.
 * Otherwise, an OID will be assigned (if necessary) by heap_insert.
 * ----------------
 */
void
InsertOneTuple(bki_parse_state *parse_state)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	int			i;

	elog(DEBUG4, "inserting row with %d columns", parse_state->numattr);

	tupDesc = CreateTupleDesc(parse_state->numattr, parse_state->attrtypes);
	tuple = heap_form_tuple(tupDesc, parse_state->values, parse_state->Nulls);
	pfree(tupDesc);				/* just free's tupDesc, not the attrtypes */

	simple_heap_insert(parse_state->boot_reldesc, tuple);
	heap_freetuple(tuple);
	elog(DEBUG4, "row inserted");

	/*
	 * Reset null markers for next tuple
	 */
	for (i = 0; i < parse_state->numattr; i++)
		parse_state->Nulls[i] = false;
}

/* ----------------
 *		InsertOneValue
 * ----------------
 */
void
InsertOneValue(bki_parse_state *parse_state, char *value, int i)
{
	Form_pg_attribute attr;
	Oid			typoid;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	Oid			typioparam;
	Oid			typinput;
	Oid			typoutput;
	Oid			typcollation;

	Assert(i >= 0 && i < MAXATTR);

	elog(DEBUG4, "inserting column %d value \"%s\"", i, value);

	attr = TupleDescAttr(RelationGetDescr(parse_state->boot_reldesc), i);
	typoid = attr->atttypid;

	boot_get_type_io_data(typoid,
						  &typlen, &typbyval, &typalign,
						  &typdelim, &typioparam,
						  &typinput, &typoutput,
						  &typcollation);

	/*
	 * pg_node_tree values can't be inserted normally (pg_node_tree_in would
	 * just error out), so provide special cases for such columns that we
	 * would like to fill during bootstrap.
	 */
	if (typoid == PG_NODE_TREEOID)
	{
		/* pg_proc.proargdefaults */
		if (RelationGetRelid(parse_state->boot_reldesc) == ProcedureRelationId &&
			i == Anum_pg_proc_proargdefaults - 1)
			InsertOneProargdefaultsValue(parse_state, value);
		else					/* maybe other cases later */
			elog(ERROR, "can't handle pg_node_tree input for %s.%s",
				 RelationGetRelationName(parse_state->boot_reldesc),
				 NameStr(attr->attname));
	}
	else
	{
		/* Normal case */
		parse_state->values[i] = OidInputFunctionCall(typinput, value, typioparam, -1);
	}

	/*
	 * We use ereport not elog here so that parameters aren't evaluated unless
	 * the message is going to be printed, which generally it isn't
	 */
	ereport(DEBUG4,
			(errmsg_internal("inserted -> %s",
							 OidOutputFunctionCall(typoutput, parse_state->values[i]))));
}

/* ----------------
 *		InsertOneProargdefaultsValue
 *
 * In general, proargdefaults can be a list of any expressions, but
 * for bootstrap we only support a list of Const nodes.  The input
 * has the form of a text array, and we feed non-null elements to the
 * typinput functions for the appropriate parameters.
 * ----------------
 */
static void
InsertOneProargdefaultsValue(bki_parse_state *parse_state, char *value)
{
	int			pronargs;
	oidvector  *proargtypes;
	Datum		arrayval;
	Datum	   *array_datums;
	bool	   *array_nulls;
	int			array_count;
	List	   *proargdefaults;
	char	   *nodestring;

	/* The pg_proc columns we need to use must have been filled already */
	StaticAssertDecl(Anum_pg_proc_pronargs < Anum_pg_proc_proargdefaults,
					 "pronargs must come before proargdefaults");
	StaticAssertDecl(Anum_pg_proc_pronargdefaults < Anum_pg_proc_proargdefaults,
					 "pronargdefaults must come before proargdefaults");
	StaticAssertDecl(Anum_pg_proc_proargtypes < Anum_pg_proc_proargdefaults,
					 "proargtypes must come before proargdefaults");
	if (parse_state->Nulls[Anum_pg_proc_pronargs - 1])
		elog(ERROR, "pronargs must not be null");
	if (parse_state->Nulls[Anum_pg_proc_proargtypes - 1])
		elog(ERROR, "proargtypes must not be null");
	pronargs = DatumGetInt16(parse_state->values[Anum_pg_proc_pronargs - 1]);
	proargtypes = DatumGetPointer(parse_state->values[Anum_pg_proc_proargtypes - 1]);
	Assert(pronargs == proargtypes->dim1);

	/* Parse the input string as an array value, then deconstruct to Datums */
	arrayval = OidFunctionCall3(F_ARRAY_IN,
								CStringGetDatum(value),
								ObjectIdGetDatum(CSTRINGOID),
								Int32GetDatum(-1));
	deconstruct_array_builtin(DatumGetArrayTypeP(arrayval), CSTRINGOID,
							  &array_datums, &array_nulls, &array_count);

	/* The values should correspond to the last N argtypes */
	if (array_count > pronargs)
		elog(ERROR, "too many proargdefaults entries");

	/* Build the List of Const nodes */
	proargdefaults = NIL;
	for (int i = 0; i < array_count; i++)
	{
		Oid			argtype = proargtypes->values[pronargs - array_count + i];
		int16		typlen;
		bool		typbyval;
		char		typalign;
		char		typdelim;
		Oid			typioparam;
		Oid			typinput;
		Oid			typoutput;
		Oid			typcollation;
		Datum		defval;
		bool		defnull;
		Const	   *defConst;

		boot_get_type_io_data(argtype,
							  &typlen, &typbyval, &typalign,
							  &typdelim, &typioparam,
							  &typinput, &typoutput,
							  &typcollation);

		defnull = array_nulls[i];
		if (defnull)
			defval = (Datum) 0;
		else
			defval = OidInputFunctionCall(typinput,
										  DatumGetCString(array_datums[i]),
										  typioparam, -1);

		defConst = makeConst(argtype,
							 -1,	/* never any typmod */
							 typcollation,
							 typlen,
							 defval,
							 defnull,
							 typbyval);
		proargdefaults = lappend(proargdefaults, defConst);
	}

	/*
	 * Flatten the List to a node-tree string, then convert to a text datum,
	 * which is the storage representation of pg_node_tree.
	 */
	nodestring = nodeToString(proargdefaults);
	parse_state->values[Anum_pg_proc_proargdefaults - 1] = CStringGetTextDatum(nodestring);
	parse_state->Nulls[Anum_pg_proc_proargdefaults - 1] = false;

	/*
	 * Hack: fill in pronargdefaults with the right value.  This is surely
	 * ugly, but it beats making the programmer do it.
	 */
	parse_state->values[Anum_pg_proc_pronargdefaults - 1] = Int16GetDatum(array_count);
	parse_state->Nulls[Anum_pg_proc_pronargdefaults - 1] = false;
}

/* ----------------
 *		InsertOneNull
 * ----------------
 */
void
InsertOneNull(bki_parse_state *parse_state, int i)
{
	elog(DEBUG4, "inserting column %d NULL", i);
	Assert(i >= 0 && i < MAXATTR);
	if (TupleDescAttr(parse_state->boot_reldesc->rd_att, i)->attnotnull)
		elog(ERROR,
			 "NULL value specified for not-null column \"%s\" of relation \"%s\"",
			 NameStr(TupleDescAttr(parse_state->boot_reldesc->rd_att, i)->attname),
			 RelationGetRelationName(parse_state->boot_reldesc));
	parse_state->values[i] = PointerGetDatum(NULL);
	parse_state->Nulls[i] = true;
}

/* ----------------
 *		cleanup
 * ----------------
 */
static void
cleanup(bki_parse_state *parse_state)
{
	if (parse_state->boot_reldesc != NULL)
		closerel(parse_state, NULL);
}

/* ----------------
 *		populate_typ_list
 *
 * Load the Typ list by reading pg_type.
 * ----------------
 */
static void
populate_typ_list(void)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext old;

	elog(LOG, "POPULATE");

	Assert(Typ == NIL);

	rel = table_open(TypeRelationId, NoLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	old = MemoryContextSwitchTo(TopMemoryContext);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_type typForm = (Form_pg_type) GETSTRUCT(tup);
		struct typinfo *newtyp;

		newtyp = palloc_object(struct typinfo);
		newtyp->oid = typForm->oid;
		/* NB: We need to zero-pad the destination. */
		strncpy(newtyp->typname, NameStr(typForm->typname), NAMEDATALEN);
		newtyp->oid = typForm->oid;
		newtyp->typelem = typForm->typelem;
		newtyp->typlen = typForm->typlen;
		newtyp->typbyval = typForm->typbyval;
		newtyp->typalign = typForm->typalign;
		newtyp->typstorage = typForm->typstorage;
		newtyp->typcollation = typForm->typcollation;
		newtyp->typinput = typForm->typinput;
		newtyp->typoutput = typForm->typoutput;

		Typ = lappend(Typ, newtyp);
	}
	MemoryContextSwitchTo(old);
	table_endscan(scan);
	table_close(rel, NoLock);
}

/* ----------------
 *		gettype
 *
 * NB: this is really ugly; it will return an integer index into TypInfo[],
 * and not an OID at all, until the first reference to a type not known in
 * TypInfo[].  At that point it will read and cache pg_type in Typ,
 * and subsequently return a real OID (and set the global pointer Ap to
 * point at the found row in Typ).  So caller must check whether Typ is
 * still NIL to determine what the return value is!
 * ----------------
 */
static const struct typinfo *
get_builtin_type(char *type)
{
	elog(LOG, "get_builtin_type(%s)", type);
	for (int i = 0; i < n_types; i++)
	{
		if (strncmp(type, TypInfo[i].typname, NAMEDATALEN) == 0)
			return &TypInfo[i];
	}
	return NULL;
}

static struct typinfo *
get_type(char *type)
{
	elog(LOG, "get_type(%s)", type);
	Assert(Typ != NIL);

	foreach_ptr(struct typinfo, app, Typ)
	{
		if (strncmp(app->typname, type, NAMEDATALEN) == 0)
			return app;
	}
	return NULL;
}

/* ----------------
 *		boot_get_type_io_data
 *
 * Obtain type I/O information at bootstrap time.  This intentionally has
 * an API very close to that of lsyscache.c's get_type_io_data, except that
 * we only support obtaining the typinput and typoutput routines, not
 * the binary I/O routines, and we also return the type's collation.
 * This is exported so that array_in and array_out can be made to work
 * during early bootstrap.
 * ----------------
 */
void
boot_get_type_io_data(Oid typid,
					  int16 *typlen,
					  bool *typbyval,
					  char *typalign,
					  char *typdelim,
					  Oid *typioparam,
					  Oid *typinput,
					  Oid *typoutput,
					  Oid *typcollation)
{
	const struct typinfo *tp = NULL;

	elog(LOG, "boot_get_type_io_data(%u)", typid);

	if (Typ != NIL)
	{
		/* We have the boot-time contents of pg_type, so use it */
		ListCell   *lc;

		foreach(lc, Typ)
		{
			tp = lfirst(lc);
			if (tp->oid == typid)
				break;
		}

		if (!tp || tp->oid != typid)
			elog(ERROR, "type OID %u not found in Typ list", typid);
	}
	else
	{
		/* We don't have pg_type yet, so use the hard-wired TypInfo array */
		int			typeindex;

		for (typeindex = 0; typeindex < n_types; typeindex++)
		{
			if (TypInfo[typeindex].oid == typid)
			{
				tp = &TypInfo[typeindex];
				break;
			}
		}
		if (typeindex >= n_types)
			elog(ERROR, "type OID %u not found in TypInfo", typid);
	}

	*typlen = tp->typlen;
	*typbyval = tp->typbyval;
	*typalign = tp->typalign;
	/* We assume typdelim is ',' for all boot-time types */
	*typdelim = Typ == NIL ? ',': tp->typdelim;

	/* XXX this logic must match getTypeIOParam() */
	if (OidIsValid(tp->typelem))
		*typioparam = tp->typelem;
	else
		*typioparam = typid;

	*typinput = tp->typinput;
	*typoutput = tp->typoutput;
	*typcollation = tp->typcollation;
}

/* ----------------
 *		boot_get_role_oid
 *
 * Look up a role name at bootstrap time.  This is equivalent to
 * get_role_oid(rolname, true): return the role OID or InvalidOid if
 * not found.  We only need to cope with built-in role names.
 * ----------------
 */
Oid
boot_get_role_oid(const char *rolname)
{
	for (int i = 0; i < lengthof(RolInfo); i++)
	{
		if (strcmp(RolInfo[i].rolname, rolname) == 0)
			return RolInfo[i].oid;
	}
	return InvalidOid;
}

/* ----------------
 *		AllocateAttribute
 *
 * Note: bootstrap never sets any per-column ACLs, so we only need
 * ATTRIBUTE_FIXED_PART_SIZE space per attribute.
 * ----------------
 */
static Form_pg_attribute
AllocateAttribute(void)
{
	return (Form_pg_attribute)
		MemoryContextAllocZero(TopMemoryContext, ATTRIBUTE_FIXED_PART_SIZE);
}

/*
 *	index_register() -- record an index that has been set up for building
 *						later.
 *
 *		At bootstrap time, we define a bunch of indexes on system catalogs.
 *		We postpone actually building the indexes until just before we're
 *		finished with initialization, however.  This is because the indexes
 *		themselves have catalog entries, and those have to be included in the
 *		indexes on those catalogs.  Doing it in two phases is the simplest
 *		way of making sure the indexes have the right contents at the end.
 */
void
index_register(Oid heap,
			   Oid ind,
			   const IndexInfo *indexInfo)
{
	IndexList  *newind;
	MemoryContext oldcxt;

	/*
	 * XXX mao 10/31/92 -- don't gc index reldescs, associated info at
	 * bootstrap time.  we'll declare the indexes now, but want to create them
	 * later.
	 */

	if (nogc == NULL)
		nogc = AllocSetContextCreate(NULL,
									 "BootstrapNoGC",
									 ALLOCSET_DEFAULT_SIZES);

	oldcxt = MemoryContextSwitchTo(nogc);

	newind = palloc_object(IndexList);
	newind->il_heap = heap;
	newind->il_ind = ind;
	newind->il_info = palloc_object(IndexInfo);

	memcpy(newind->il_info, indexInfo, sizeof(IndexInfo));
	/* expressions will likely be null, but may as well copy it */
	newind->il_info->ii_Expressions =
		copyObject(indexInfo->ii_Expressions);
	newind->il_info->ii_ExpressionsState = NIL;
	/* predicate will likely be null, but may as well copy it */
	newind->il_info->ii_Predicate =
		copyObject(indexInfo->ii_Predicate);
	newind->il_info->ii_PredicateState = NULL;
	/* no exclusion constraints at bootstrap time, so no need to copy */
	Assert(indexInfo->ii_ExclusionOps == NULL);
	Assert(indexInfo->ii_ExclusionProcs == NULL);
	Assert(indexInfo->ii_ExclusionStrats == NULL);

	newind->il_next = ILHead;
	ILHead = newind;

	MemoryContextSwitchTo(oldcxt);
}


/*
 * build_indices -- fill in all the indexes registered earlier
 */
void
build_indices(bki_parse_state *parse_state)
{
	for (; ILHead != NULL; ILHead = ILHead->il_next)
	{
		Relation	heap;
		Relation	ind;

		/* need not bother with locks during bootstrap */
		heap = table_open(ILHead->il_heap, NoLock);
		ind = index_open(ILHead->il_ind, NoLock);

		index_build(heap, ind, ILHead->il_info, false, false, false);

		index_close(ind, NoLock);
		table_close(heap, NoLock);
	}
}
