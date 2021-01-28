MODULE_big = test_conversion
OBJS = test_conversion.o

EXTENSION = test_conversion
DATA = test_conversion--1.0.sql
PGFILEDESC = "test_conversion - sample user-defined encoding conversion"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/test_conversion
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
