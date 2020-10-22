#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>

#include <libpq-fe.h>

static PGconn *conn;

static void
execute(const char *sql)
{
	int			i;
	PGresult   *res;

	fprintf(stderr, "%s\n", sql);
	
	res = PQexec(conn, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr,"command failed: %s\n%s", sql, PQerrorMessage(conn));
		PQclear(res);
		exit(1);
	}

	PQclear(res);
}

static void
check_sorted(const char *sql, int (*cmp)(const char *a, const char *b))
{
	int			i;
	PGresult   *res;
	PGresult   *prevres = NULL;
	int			rowno;

	fprintf(stderr, "running query: %s\n", sql);
	if (!PQsendQuery(conn, sql))
	{
		fprintf(stderr,"query failed: %s\n%s", sql, PQerrorMessage(conn));
		PQclear(res);
		exit(1);
	}
	if (!PQsetSingleRowMode(conn))
	{
		fprintf(stderr,"setting single-row mode failed: %s", PQerrorMessage(conn));
		PQclear(res);
		exit(1);
	}

	rowno = 1;
	while (res = PQgetResult(conn))
	{
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
			continue;
		if (PQresultStatus(res) != PGRES_SINGLE_TUPLE)
		{
			fprintf(stderr,"error while fetching: %d, %s\n%s", PQresultStatus(res), sql, PQerrorMessage(conn));
			PQclear(res);
			exit(1);
		}

		if (prevres)
		{
			if (!cmp(PQgetvalue(prevres, 0, 0), PQgetvalue(res, 0, 0)))
			{
				fprintf(stderr,"FAIL: result not sorted, row %d: %s, prev %s\n", rowno,
						PQgetvalue(prevres, 0, 0), PQgetvalue(res, 0, 0));
				PQclear(res);
				exit(1);
			}
			PQclear(prevres);
		}
		prevres = res;

		rowno++;
	}

	if (prevres)
		PQclear(prevres);
}


static int
compare_strings(const char *a, const char *b)
{
	return strcmp(a, b) <= 0;
}

static int
compare_ints(const char *a, const char *b)
{
	return atoi(a) <= atoi(b);
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
	execute("set trace_sort=on");

	execute("set work_mem = '4MB'");

	check_sorted("SELECT * FROM small.ordered_ints ORDER BY i", compare_ints);
	check_sorted("SELECT * FROM small.random_ints ORDER BY i", compare_ints);
	check_sorted("SELECT * FROM small.ordered_text ORDER BY t", compare_strings);
	check_sorted("SELECT * FROM small.random_text ORDER BY t", compare_strings);

	execute("set work_mem = '16MB'");

	check_sorted("SELECT * FROM medium.ordered_ints ORDER BY i", compare_ints);
	check_sorted("SELECT * FROM medium.random_ints ORDER BY i", compare_ints);
	check_sorted("SELECT * FROM medium.ordered_text ORDER BY t", compare_strings);
	check_sorted("SELECT * FROM medium.random_text ORDER BY t", compare_strings);

	execute("set work_mem = '256MB'");

	check_sorted("SELECT * FROM medium.ordered_ints ORDER BY i", compare_ints);
	check_sorted("SELECT * FROM medium.random_ints ORDER BY i", compare_ints);
	check_sorted("SELECT * FROM medium.ordered_text ORDER BY t", compare_strings);
	check_sorted("SELECT * FROM medium.random_text ORDER BY t", compare_strings);

	execute("set work_mem = '512MB'");

	check_sorted("SELECT * FROM medium.ordered_ints ORDER BY i", compare_ints);
	check_sorted("SELECT * FROM medium.random_ints ORDER BY i", compare_ints);
	check_sorted("SELECT * FROM medium.ordered_text ORDER BY t", compare_strings);
	check_sorted("SELECT * FROM medium.random_text ORDER BY t", compare_strings);

	execute("set work_mem = '2048MB'");

	check_sorted("SELECT * FROM medium.ordered_ints ORDER BY i", compare_ints);
	check_sorted("SELECT * FROM medium.random_ints ORDER BY i", compare_ints);
	check_sorted("SELECT * FROM medium.ordered_text ORDER BY t", compare_strings);
	check_sorted("SELECT * FROM medium.random_text ORDER BY t", compare_strings);

	PQfinish(conn);

	return 0;
}
