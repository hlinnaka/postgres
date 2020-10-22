#define FRONTEND 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>

#define pg_attribute_printf(f,a)

#include <libpq-fe.h>
#include <internal/pqexpbuffer.h>

#define REPETITIONS 1

static PGconn *conn;

/* returns duration in ms */
static double
execute(const char *sql)
{
	struct timeval before, after;
	PGresult   *res;

	gettimeofday(&before, NULL);
	res = PQexec(conn, sql);
	gettimeofday(&after, NULL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr,"command failed: %s\n%s", sql, PQerrorMessage(conn));
		PQclear(res);
		exit(1);
	}
	PQclear(res);

	return (((double) (after.tv_sec - before.tv_sec)) * 1000.0 + ((double) (after.tv_usec - before.tv_usec) / 1000.0));
}

static void
execute_test(const char *testname, const char *query)
{
	double		duration;
	char		buf[1000];
	int			i;

	for (i = 0; i < REPETITIONS; i++)
	{
		printf ("%s: ", testname);
		fflush(stdout);
		duration = execute(query);

		//if (i > 0)
		//	printf(", ");
		printf("%.0f ms", duration);
		printf("\n");
		fflush(stdout);

		sprintf(buf,
				"insert into public.results (testname, work_mem, time_ms) "
				"values ('%s', (select setting from pg_settings where name='work_mem')::int, '%g')",
				testname, duration);
		execute(buf);
	}
}

static void
execute_test_series(char *tblname)
{
	//static const char *work_mems[] = { "1MB", "4MB", "8MB", "16MB", "32MB", "64MB", "128MB", "256MB", "512MB", NULL };
	static const char *work_mems[] = {
		"64 kB",
		"80 kB",

		"100 kB",
		"150 kB",
		"250 kB",
		"400 kB",
		"650 kB",

		"1000 kB",
		"1500 kB",
		"2500 kB",
		"4000 kB",
		"6500 kB",

		"10000 kB",
		"15000 kB",
		"25000 kB",
		"40000 kB",
		"65000 kB",

		"100 MB",
		"150 MB",
		"250 MB",
		"400 MB",
		"650 MB",
		"800 MB",

		"1000 MB",
		"1500 MB",
		"2500 MB",
		
		NULL, NULL, NULL, NULL };
	int			i;
	int			j;
	PQExpBufferData sqlbuf;

	initPQExpBuffer(&sqlbuf);

	printf("# Tests with %s, different work_mems, no parallelism\n", tblname);
	printf("-----\n");

	printfPQExpBuffer(&sqlbuf, "set max_parallel_workers_per_gather=0");
	execute(sqlbuf.data);
	
	for (i = 0; work_mems[i] != NULL; i++)
	{
		const char *work_mem = work_mems[i];
		char testname[100];

 		printfPQExpBuffer(&sqlbuf, "set work_mem='%s'", work_mem);
		execute(sqlbuf.data);
		snprintf(testname, sizeof(testname), "%s", tblname, work_mem);

		resetPQExpBuffer(&sqlbuf);

		appendPQExpBuffer(&sqlbuf, "SELECT COUNT(*) FROM ((");
		appendPQExpBuffer(&sqlbuf, "SELECT * FROM %s", tblname);
		appendPQExpBuffer(&sqlbuf, ") ORDER BY 1) t");

		execute_test(testname,  sqlbuf.data);
	}
}

int
main(int argc, char **argv)
{
	double duration;
	char		buf[1000];

	/* Make a connection to the database */
	conn = PQconnectdb("");

	/* Check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "Connection to database failed: %s",
				PQerrorMessage(conn));
		exit(1);
	}

	execute("create table if not exists public.results (testname text, work_mem int, time_ms numeric)");

	//execute("set trace_sort=on");
#if 0
	execute_test_series("small.ordered_ints");
	execute_test_series("small.random_ints");
	execute_test_series("small.ordered_text");
	execute_test_series("small.random_text");
#endif
	
	execute_test_series("medium.ordered_ints");
	execute_test_series("medium.random_ints");
	execute_test_series("medium.ordered_text");
	execute_test_series("medium.random_text");

#if 0
	execute_test_series("large.ordered_ints");
	execute_test_series("large.random_ints");
	execute_test_series("large.ordered_text");
	execute_test_series("large.random_text");
#endif
	
	PQfinish(conn);

	return 0;
}
