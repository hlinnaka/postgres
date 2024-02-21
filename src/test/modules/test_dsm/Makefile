# src/test/modules/test_dsm/Makefile

MODULE_big = test_dsm
OBJS = \
	$(WIN32RES) \
	test_dsm.o
PGFILEDESC = "test_dsm - test code for dynamic shared memory"

EXTENSION = test_dsm
DATA = test_dsm--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_dsm
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
