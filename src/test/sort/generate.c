#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
generate_ordered_integers(int scale)
{
	int			rows = ((double) scale) * 28.75;
	int			i;

	printf("DROP TABLE IF EXISTS ordered_ints;\n");
	printf("BEGIN;");
	printf("CREATE TABLE ordered_ints (i int4);\n");
	printf("COPY ordered_ints FROM STDIN WITH (FREEZE);\n");

	for (i = 0; i < rows; i++)
		printf("%d\n", i);

	printf("\\.\n");
	printf("COMMIT;\n");
}

static void
generate_random_integers(int scale)
{
	int			rows = ((double) scale) * 28.75;
	int			i;

	printf("DROP TABLE IF EXISTS random_ints;\n");
	printf("BEGIN;");
	printf("CREATE TABLE random_ints (i int4);\n");
	printf("COPY random_ints FROM STDIN WITH (FREEZE);\n");

	for (i = 0; i < rows; i++)
		printf("%d\n", random());

	printf("\\.\n");
	printf("COMMIT;\n");
}

#define ALPHABET_SIZE 26
static const char alphabet[ALPHABET_SIZE + 1] = "abcdefghijklmnopqrstuvwxyz";

#define TEXT_LEN 50

static void
random_string(char *buf, int len)
{
	int			i;
	long		r;
	long		m;

	m = 0;
	for (i = 0; i < len; i++)
	{
		if (m / ALPHABET_SIZE < ALPHABET_SIZE)
		{
			m = RAND_MAX;
			r = random();
		}

		*buf = alphabet[r % ALPHABET_SIZE];
		m = m / ALPHABET_SIZE;
		r = r / ALPHABET_SIZE;
		buf++;
	}
	*buf = '\0';
	return;
}

static void
generate_random_text(int scale)
{
	int			rows = ((double) scale) * 12.7;
	int			i;
	char		buf[TEXT_LEN + 1] = { 0 };

	printf("DROP TABLE IF EXISTS random_text;\n");
	printf("BEGIN;");
	printf("CREATE TABLE random_text (t text);\n");
	printf("COPY random_text FROM STDIN WITH (FREEZE);\n");

	for (i = 0; i < rows; i++)
	{
		random_string(buf, TEXT_LEN);
		printf("%s\n", buf);
	}

	printf("\\.\n");
	printf("COMMIT;\n");
}

static void
generate_ordered_text(int scale)
{
	int			rows = ((double) scale) * 12.7;
	int			i;
	int			j;
	char		indexes[TEXT_LEN] = {0};
	char		buf[TEXT_LEN + 1];
	double			digits;

	printf("DROP TABLE IF EXISTS ordered_text;\n");
	printf("BEGIN;");
	printf("CREATE TABLE ordered_text (t text);\n");
	printf("COPY ordered_text FROM STDIN WITH (FREEZE);\n");

	/*
	 * We don't want all the strings to have the same prefix.
	 * That makes the comparisons very expensive. That might be an
	 * interesting test case too, but not what we want here. To avoid
	 * that, figure out how many characters will change, with the #
	 * of rows we chose.
	 */
	digits = ceil(log(rows) / log((double) ALPHABET_SIZE));

	if (digits > TEXT_LEN)
		digits = TEXT_LEN;

	for (i = 0; i < rows; i++)
	{
		for (j = 0; j < TEXT_LEN; j++)
		{
			buf[j] = alphabet[indexes[j]];
		}
		buf[j] = '\0';
		printf("%s\n", buf);

		/* increment last character, carrying if needed */
		for (j = digits - 1; j >= 0; j--)
		{
			indexes[j]++;
			if (indexes[j] == ALPHABET_SIZE)
				indexes[j] = 0;
			else
				break;
		}
	}

	printf("\\.\n");
	printf("COMMIT;\n");
}


struct
{
	char *name;
	void (*generate_func)(int scale);
} datasets[] =
{
 	{ "ordered_integers", generate_ordered_integers },
	{ "random_integers", generate_random_integers },
	{ "ordered_text", generate_ordered_text },
	{ "random_text", generate_random_text },
	{ NULL, NULL }
};

void
usage()
{
	printf("Usage: generate <dataset name> [scale] [schema]\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	int			scale;
	int			i;
	int			found = 0;

	if (argc < 2)
		usage();

	if (argc >= 3)
		scale = atoi(argv[2]);
	else
		scale = 1024; /* 1 MB */

	for (i = 0; datasets[i].name != NULL; i++)
	{
		if (strcmp(argv[1], datasets[i].name) == 0 ||
			strcmp(argv[1], "all") == 0)
		{
			fprintf (stderr, "Generating %s for %d kB...\n", datasets[i].name, scale);
			datasets[i].generate_func(scale);
			found = 1;
		}
	}

	if (!found)
	{
		fprintf(stderr, "unrecognized test name %s\n", argv[1]);
		exit(1);
	}
	exit(0);
}
