CFLAGS=-g -I`pg_config --includedir`

LDFLAGS=-L`pg_config --libdir` -lpq -lm

TESTDB=sorttest

# For testing quicksort.
SCALE_SMALL=1024	# 1 MB

# For testing external sort, while the dataset still fits in OS cache.
SCALE_MEDIUM=1048576	# 1 GB
#SCALE_MEDIUM=102400	# 100 MB

# Does not fit in memory.
SCALE_LARGE=20971520	# 20 GB
#SCALE_LARGE=1500000	# 20 GB

all: generate speed correctness

generate: generate.c

speed: speed.c

correctness: correctness.c

generate_testdata:
	dropdb --if-exists $(TESTDB)
	createdb $(TESTDB)
	psql $(TESTDB) -c "CREATE SCHEMA small; CREATE SCHEMA medium; CREATE SCHEMA large;"
	(echo "set search_path=medium;"; ./generate all $(SCALE_MEDIUM)) | psql $(TESTDB)
	(echo "set search_path=small;"; ./generate all $(SCALE_SMALL)) | psql $(TESTDB)
	(echo "set search_path=large;"; ./generate all $(SCALE_LARGE)) | psql $(TESTDB)
