/*-------------------------------------------------------------------------
 *
 * pgguclifetimes.c
 *
 * Static source analyzer for finding global variable declarations without GUC
 * lifetime annotations.
 *
 * Copyright (c) 2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/tools/pgguclifetimes/pgguclifetimes.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "getopt.h"

#include <unistd.h>

#include <clang-c/CXCompilationDatabase.h>
#include <clang-c/Index.h>

#include "vector.h"

struct field_iterator {
	unsigned int curr;
	unsigned int want;
};

struct parse_context {
	int err;
	void *client_data;
};

static const char *guc_lifetimes[] = {
	"dynamic_singleton",
	"global",
	"internal_guc",
	"postmaster_guc",
	"session_guc",
	"session_local",
	"sighup_guc",
	"static_singleton",
	"suset_guc",
	"userset_guc",
};
static const char *compdb_dir = NULL;
static bool fail_fast = false;
static bool quiet = false;
static struct vector excludes = { 0 };
static struct vector includes = { 0 };
static struct vector gucs = { 0 };

static enum CXChildVisitResult
visit_guc_lifetime(CXCursor cursor, CXCursor parent, CXClientData client_data)
{
	bool *annotated;
	CXString guc_lifetime;
	struct parse_context *ctx;
	const char *guc_lifetime_c;

	Assert(client_data != NULL);

	ctx = client_data;
	annotated = ctx->client_data;

	guc_lifetime = clang_getCursorSpelling(cursor);
	guc_lifetime_c = clang_getCString(guc_lifetime);

	for (size_t i = 0; i < lengthof(guc_lifetimes); i++) {
		if (strcmp(guc_lifetime_c, guc_lifetimes[i]) == 0) {
			*annotated = true;
			break;
		}
	}

	clang_disposeString(guc_lifetime);

	return *annotated ? CXChildVisit_Break : CXChildVisit_Continue;
}

static enum CXChildVisitResult
visit_annotation(CXCursor cursor, CXCursor parent, CXClientData client_data)
{
	Assert(parent.kind == CXCursor_VarDecl);

	switch (cursor.kind) {
	case CXCursor_AnnotateAttr:
		return visit_guc_lifetime(cursor, parent, client_data);
	default:
		return CXChildVisit_Continue;
	}
}

static int pg_attribute_printf(2, 3)
error(CXCursor cursor, const char *fmt, ...)
{
	int n = 0;
	va_list ap;
	CXFile file;
	CXString variable;
	CXString filename;
	unsigned int line;
	unsigned int column;
	CXSourceLocation location;

	if (quiet)
		return 0;

	variable = clang_getCursorSpelling(cursor);
	location = clang_getCursorLocation(cursor);

	clang_getFileLocation(location, &file, &line, &column, NULL);
	filename = clang_getFileName(file);

	n += fprintf(stderr, "%s:%u:%u: %s ",
		clang_getCString(filename), line, column, clang_getCString(variable));

	va_start(ap, fmt);
	n += vfprintf(stderr, fmt, ap);
	va_end(ap);

	n += fputc('\n', stderr);

	clang_disposeString(filename);
	clang_disposeString(variable);

	return n;
}

static enum CXChildVisitResult
visit_global_variable(CXCursor cursor, CXCursor parent, CXClientData client_data)
{
	CXString variable;
	bool global_storage;
	unsigned int *issues;
	bool annotated = false;
	const char *variable_c;
	struct parse_context *ctx;
	CXSourceLocation location;

	Assert(client_data != NULL);

	/* Keep analysis within the main file. */
    location = clang_getCursorLocation(cursor);
	if (!clang_Location_isFromMainFile(location))
		return CXChildVisit_Continue;

	global_storage = clang_Cursor_hasVarDeclGlobalStorage(cursor);
	if (!global_storage)
		return CXChildVisit_Continue;

	variable = clang_getCursorSpelling(cursor);
	variable_c = clang_getCString(variable);

	/* We can't annotate flex/bison generated code. */
	if (variable_c[0] == 'y' && variable_c[1] == 'y') {
		clang_disposeString(variable);

		return CXChildVisit_Continue;
	}

	/*
	 * We don't need to analyze GUCs because their lifetimes are annotated in
	 * guc_tables.c.
	 */
	for (size_t i = 0; i < gucs.len; i++) {
		if (strcmp(variable_c, gucs.data[i]) == 0) {
			clang_disposeString(variable);

			return CXChildVisit_Continue;
		}
	}

	clang_disposeString(variable);

	ctx = client_data;
	issues = ctx->client_data;
	ctx->client_data = &annotated;

	clang_visitChildren(cursor, visit_annotation, ctx);

	if (!annotated) {
		(*issues)++;
		error(cursor, "is missing a lifetime annotation");
	}

	/* Restore the original data. */
	ctx->client_data = issues;

	return fail_fast && !annotated ? CXChildVisit_Break : CXChildVisit_Continue;
}

static enum CXChildVisitResult
visit_translation_unit(CXCursor cursor, CXCursor parent, CXClientData client_data)
{
	switch (cursor.kind) {
		case CXCursor_VarDecl:
			return visit_global_variable(cursor, parent, client_data);
		default:
			return CXChildVisit_Continue;
	}
}

static enum CXChildVisitResult
visit_initializer(CXCursor cursor, CXCursor parent, CXClientData client_data)
{
	char *suffix;
	char *field_c;
	CXString field;
	struct parse_context *ctx;

	Assert(client_data != NULL);

	ctx = client_data;

	/* This is the only case we know of at the moment. */
	if (cursor.kind != CXCursor_DeclRefExpr)
		abort();

	field = clang_getCursorSpelling(cursor);
	field_c = strdup(clang_getCString(field));
	if (field_c == NULL) {
		ctx->err = ENOMEM;
		goto out;
	}

	/* Drop the suffix. */
	suffix = strstr(field_c, "_address");
	suffix[0] = '\0';

	if (!vector_append(&gucs, field_c)) {
		ctx->err = ENOMEM;
		goto out;
	}

out:
	clang_disposeString(field);

	return CXChildVisit_Break;
}

static enum CXChildVisitResult
visit_guc_fields(CXCursor cursor, CXCursor parent, CXClientData client_data)
{
	struct parse_context *ctx;
	struct field_iterator *iter;

	Assert(client_data != NULL);

	ctx = client_data;
	iter = ctx->client_data;

	if (iter->curr == iter->want) {
		switch (cursor.kind) {
		case CXCursor_UnaryOperator:
			clang_visitChildren(cursor, visit_initializer, ctx);
			break;
		default:
			/* Only known case is the NULL terminator of the arrays. */
			break;
		}

		return CXChildVisit_Break;
	}

	iter->curr++;

	return CXChildVisit_Continue;
}

static enum CXChildVisitResult
visit_guc(CXCursor cursor, CXCursor parent, CXClientData client_data)
{
	void *save;
	struct parse_context *ctx;
	struct field_iterator iter;

	Assert(client_data != NULL);

	ctx = client_data;

	iter.curr = 0;
	iter.want = *(unsigned int *) ctx->client_data;

	save = ctx->client_data;
	ctx->client_data = &iter;

	switch (cursor.kind) {
	case CXCursor_InitListExpr:
		clang_visitChildren(cursor, visit_guc_fields, ctx);
		break;
	default:
		break;
	}

	ctx->client_data = save;

	return CXChildVisit_Continue;
}

static enum CXChildVisitResult
visit_guc_table(CXCursor cursor, CXCursor parent, CXClientData client_data)
{
	switch (cursor.kind) {
	case CXCursor_InitListExpr:
		clang_visitChildren(cursor, visit_guc, client_data);
		break;
	default:
		break;
	}

	return CXChildVisit_Continue;
}

static enum CXVisitorResult
visit_config_type(CXCursor cursor, CXClientData client_data)
{
	CXString field;
	struct parse_context *ctx;
	unsigned int *field_index;
	enum CXVisitorResult result;

	Assert(client_data != NULL);

	ctx = client_data;
	field_index = ctx->client_data;

	field = clang_getCursorSpelling(cursor);
	if (strcmp(clang_getCString(field), "variable_addr") == 0) {
		result = CXVisit_Break;
	} else {
		result = CXVisit_Continue;
		(*field_index)++;
	}

	clang_disposeString(field);

	return result;
}

static enum CXChildVisitResult
visit_guc_tables(CXCursor cursor, CXCursor parent, CXClientData client_data)
{
	static const char *guc_tables[] = {
		"ConfigureNamesBool",
		"ConfigureNamesEnum",
		"ConfigureNamesInt",
		"ConfigureNamesReal",
		"ConfigureNamesString",
	};

	void *save;
	CXType type;
	CXString variable;
	bool global_storage;
	const char *variable_c;
	struct parse_context *ctx;
	unsigned int field_index = 0;

	Assert(client_data != NULL);

	ctx = client_data;
	save = ctx->client_data;
	ctx->client_data = &field_index;

	switch (cursor.kind) {
	case CXCursor_VarDecl:
		global_storage = clang_Cursor_hasVarDeclGlobalStorage(cursor);
		if (!global_storage)
			return CXChildVisit_Continue;

		type = clang_getCursorType(cursor);
		type = clang_getElementType(type);

		variable = clang_getCursorSpelling(cursor);
		variable_c = clang_getCString(variable);

		for (size_t i = 0; i < lengthof(guc_tables); i++) {
			if (strcmp(variable_c, guc_tables[i]) == 0) {
				clang_Type_visitFields(type, visit_config_type, ctx);
				clang_visitChildren(cursor, visit_guc_table, ctx);

				break;
			}
		}

		clang_disposeString(variable);

		break;
	default:
		break;
	}

	ctx->client_data = save;

	return CXChildVisit_Continue;
}

static bool
ignore_arg(const char *arg)
{
	if (strstr(arg, "-W") == arg)
		return true;

	return false;
}

static bool
copy_args(CXCompileCommand command, struct vector *args)
{
	unsigned int nargs;

	Assert(command != NULL);
	Assert(args != NULL);

	/*
	 * Ninja will use a relative path as the last argument of the compile
	 * command when generating the compilation database because that is what
	 * Meson supplies it with. We will supply an absolute path to libclang
	 * later on, so skip the last argument.
	 */
	nargs = clang_CompileCommand_getNumArgs(command) - 1;

	if (!vector_init(args, nargs))
		return false;

	for (unsigned int i = 0; i < nargs; i++) {
		char *copy;
		CXString arg;
		const char *arg_c;

		arg = clang_CompileCommand_getArg(command, i);
		arg_c = clang_getCString(arg);

		/* Ignore various arguments that don't help libclang parse the code. */
		if (ignore_arg(arg_c)) {
			clang_disposeString(arg);
			continue;
		}

		copy = strdup(arg_c);
		clang_disposeString(arg);

		if (copy == NULL)
			return NULL;

		/* Infallible since we reserved enough space to begin with. */
		vector_append(args, copy);
	}

	return true;
}

static int
analyze(CXCompileCommand command, CXCursorVisitor visit, bool force, CXClientData client_data)
{
	int rc = 0;
	CXCursor cursor;
	char *filename_c;
	CXString filename;
	CXIndex index = NULL;
	enum CXErrorCode err;
	struct parse_context ctx;
	CXTranslationUnit tu = NULL;
	struct vector command_args = { 0 };

	Assert(command != NULL);

	filename = clang_CompileCommand_getFilename(command);
	filename_c = realpath(clang_getCString(filename), NULL);
	if (filename_c == NULL) {
		fprintf(stderr, "failed to resolve %s: %s\n",
			clang_getCString(filename), strerror(errno));
		return 1;
	}

	if (!force) {
		/* Skip files that should be excluded from analysis. */
		for (size_t i = 0; i < includes.len; i++) {
			const char *include;

			include = includes.data[i];
			if (include && strncmp(filename_c, include, strlen(include)) != 0)
				goto out;
		}

		/* Further exclusion... */
		for (size_t i = 0; i < excludes.len; i++) {
			const char *exclude;

			exclude = excludes.data[i];
			if (exclude && strncmp(filename_c, exclude, strlen(exclude)) == 0)
				goto out;
		}
	}

	if (!copy_args(command, &command_args)) {
		rc = 1;
		fputs("out of memory\n", stderr);
		goto out;
	}

	index = clang_createIndex(0, 0);
	if (index == NULL) {
		rc = 1;
		fputs("out of memory\n", stderr);
		goto out;
	}

	err = clang_parseTranslationUnit2FullArgv(index, filename_c,
		(const char **)command_args.data, command_args.len, NULL, 0,
		CXTranslationUnit_SkipFunctionBodies |
		CXTranslationUnit_IncludeBriefCommentsInCodeCompletion |
		CXTranslationUnit_KeepGoing, &tu);
	if (err != CXError_Success) {
		rc = 1;
		fprintf(stderr, "failed to parse the translation unit (%d)\n", err);
		goto out;
	}

	cursor = clang_getTranslationUnitCursor(tu);

	ctx.err = 0;
	ctx.client_data = client_data;
	clang_visitChildren(cursor, visit, &ctx);

	rc = ctx.err != 0;

out:
	vector_free(&command_args);
	free(filename_c);
	clang_disposeString(filename);
	clang_disposeIndex(index);
	clang_disposeTranslationUnit(tu);

	return rc;
}

static bool
read_guc_tables(CXCompilationDatabase compdb)
{
	int rc;
	unsigned int commandc;
	CXCompileCommand command;
	CXCompileCommands commandv;

	Assert(compdb != NULL);

	/* Pick something large, so we don't have to realloc(). */
	vector_init(&gucs, 512);

	/*
	 * If this fails, it means that we are probably analyzing an extension or
	 * something is seriously messed up.
	 */
	commandv = clang_CompilationDatabase_getAllCompileCommands(compdb);
	commandc = clang_CompileCommands_getSize(commandv);
	for (unsigned int i = 0; i < commandc; i++) {
		CXString filename;
		const char *filename_c;

		command = clang_CompileCommands_getCommand(commandv, i);
		filename = clang_CompileCommand_getFilename(command);
		filename_c = clang_getCString(filename);

		if (strstr(filename_c, "src/backend/utils/misc/guc_tables.c") != NULL) {
			clang_disposeString(filename);
			break;
		}

		clang_disposeString(filename);
	}

	rc = analyze(command, visit_guc_tables, true, NULL);

	clang_CompileCommands_dispose(commandv);

	return rc == 0;
}

static void
usage(const char *progname, FILE *f)
{
	fprintf(f, "pgguclifetimes is a tool for checking if GUCs have had their lifetimes annotated.\n\n");
	fprintf(f, "Usage:\n");
	fprintf(f, "  %s [OPTION]... <COMPDB_DIR>\n\n", progname);
	fprintf(f, "General options:\n");
	fprintf(f, "  -1             fail after the first error\n");
	fprintf(f, "  -e, --exclude  exclude a path\n");
	fprintf(f, "  -h, --help     show this help, then exit\n");
	fprintf(f, "  -i, --include  include a path\n");
	fprintf(f, "  -q, --quiet    output nothing on error, implies -1\n");
	fprintf(f, "\nGUC Lifetimes:\n");
	fprintf(f, "  A GUC lifetime annotation looks like:\n\n");
	fprintf(f, "      static postmaster_guc int my_global = 0;\n\n");
	fprintf(f, "  dynamic_singleton: Singleton that is set permanently at runtime\n");
	fprintf(f, "  postmaster_guc: Postmaster GUC\n");
	fprintf(f, "  session_guc: Session GUC\n");
	fprintf(f, "  session_local: Session-local global\n");
	fprintf(f, "  sighup_guc: SIGHUP GUC\n");
	fprintf(f, "  static_singleton: Singleton that is set at compile time\n");
}

int
main(const int argc, char **argv)
{
	static const struct option options[] = {
		{ "exclude", required_argument, NULL, 'e' },
		{ "help", no_argument, NULL, 'h' },
		{ "include", required_argument, NULL, 'i' },
		{ "quiet", no_argument, NULL, 'q' },
		{ 0 },
	};

	int c;
	int rc = 0;
	char *path;
	int longind;
	unsigned int issues = 0;
	struct vector files = { 0 };
	CXCompilationDatabase_Error err;
	CXCompilationDatabase compdb = NULL;

	while ((c = getopt_long(argc, argv, "+:1ehiq", options, &longind)) != -1) {
		switch (c) {
		case '1':
			fail_fast = true;
			break;
		case 'e':
			if (excludes.cap == 0) {
				if (!vector_init(&excludes, 4)) {
					rc = 1;
					fputs("out of memory\n", stderr);
					goto out;
				}
			}

			path = realpath(optarg, NULL);
			if (path == NULL) {
				rc = 1;
				fprintf(stderr, "failed to resolve %s: %s\n", optarg, strerror(errno));
				goto out;
			}

			if (!vector_append(&excludes, path)) {
				rc = 1;
				fputs("out of memory\n", stderr);
				goto out;
			}

			break;
		case 'h':
			usage(argv[0], stdout);
			goto out;
		case 'i':
			if (includes.cap == 0) {
				if (!vector_init(&includes, 4)) {
					rc = 1;
					fputs("out of memory\n", stderr);
					goto out;
				}
			}

			path = realpath(optarg, NULL);
			if (path == NULL) {
				rc = 1;
				fprintf(stderr, "failed to resolve %s: %s\n", optarg, strerror(errno));
				goto out;
			}

			if (!vector_append(&includes, path)) {
				rc = 1;
				fputs("out of memory\n", stderr);
				goto out;
			}

			break;
		case 'q':
			quiet = true;
			fail_fast = true;
			break;
		case ':':
			rc = 1;
			fprintf(stderr, "Missing required argument for -%c, --%s\n",
				options[longind].val, options[longind].name);
			usage(argv[0], stderr);
			goto out;
		case '?':
			rc = 1;
			fprintf(stderr, "Unrecognized option: %s\n", argv[optind - 1]);
			usage(argv[0], stderr);
			goto out;
		}
	}

	if (optind == argc) {
		rc = 1;
		fputs("No compilation database argument\n", stderr);
		usage(argv[0], stderr);
		goto out;
	}

	/*
	 * include/exclude normalization for cases like the following:
	 *
	 *     --include contrib/postgres_fdw --exclude contrib
	 *
	 * In this case, the --exclude is extraneous because the --include implies
	 * it, but instead of erroring, just normalize by removing contrib from
	 * the excludes path vector.
	 */
	for (size_t i = 0; i < includes.len; i++) {
		size_t include_len;
		const char *include;

		include = includes.data[i];
		include_len = strlen(include);
		for (size_t j = 0; j < excludes.len; j++) {
			char *exclude;
			size_t exclude_len;

			exclude = excludes.data[j];
			exclude_len = strlen(exclude);

			if (exclude_len <= include_len) {
				if (strncmp(exclude, include, exclude_len) == 0) {
					free(exclude);
					excludes.data[j] = NULL;
				}
			}
		}
	}

	compdb_dir = argv[optind++];

	/* We need to resolve the file paths prior to the chdir() below. */
	files.cap = argc - optind;
	if (files.cap > 0) {
		if (!vector_init(&files, files.cap)) {
			rc = 1;
			fputs("out of memory\n", stderr);
			goto out;
		}

		for (int i = 0; i < files.cap; i++, optind++) {
			char *file;

			file = realpath(argv[optind], NULL);
			if (file == NULL) {
				rc = 1;
				fprintf(stderr, "failed to resolve %s: %s\n", argv[optind], strerror(errno));
				goto out;
			}

			/* Infallible since we reserved enough space to begin with. */
			vector_append(&files, file);
		}
	}

	compdb = clang_CompilationDatabase_fromDirectory(compdb_dir, &err);
	if (err != CXCompilationDatabase_NoError) {
		rc = 1;
		goto out;
	}

	/*
	 * Change into the directory containing the compilation database to
	 * resolve relative paths.
	 */
	if (chdir(compdb_dir) == -1) {
		rc = 1;
		fprintf(stderr, "failed to change into the compilation database directory: %s\n",
			strerror(errno));
		goto out;
	}

	/* Collect names of GUC variables */
	if (!read_guc_tables(compdb)) {
		rc = 1;
		goto out;
	}

	if (files.len > 0) {
		for (int i = 0; i < files.len; i++) {
			const char *file;
			CXCompileCommand command;
			CXCompileCommands commandv;

			file = files.data[i];
			commandv = clang_CompilationDatabase_getCompileCommands(compdb, file);
			if (commandv == NULL) {
				rc = 1;
				fprintf(stderr, "failed to find %s in compilation database\n", file);
				goto out;
			}

			command = clang_CompileCommands_getCommand(commandv, 0);
			rc = analyze(command, visit_translation_unit, false, &issues);
			clang_CompileCommands_dispose(commandv);
			if (fail_fast && rc != 0)
				break;
		}
	} else {
		unsigned int commandc;
		CXCompileCommands commandv;

		commandv = clang_CompilationDatabase_getAllCompileCommands(compdb);
		commandc = clang_CompileCommands_getSize(commandv);

		for (unsigned int i = 0; i < commandc; i++) {
			CXCompileCommand command;

			command = clang_CompileCommands_getCommand(commandv, i);
			rc = analyze(command, visit_translation_unit, false, &issues);
			if (fail_fast && rc != 0)
				break;
		}

		clang_CompileCommands_dispose(commandv);
	}

out:
	clang_CompilationDatabase_dispose(compdb);

	vector_free(&excludes);
	vector_free(&includes);
	vector_free(&gucs);
	vector_free(&files);

	if (rc != 0)
		return rc;

	return issues != 0;
}
