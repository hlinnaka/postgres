#-------------------------------------------------------------------------
#
# Makefile for src/tools/pgguclifetimes
#
# Copyright (c) 2023, PostgreSQL Global Development Group
#
# src/tools/pgguclifetimes/Makefile
#
#-------------------------------------------------------------------------

subdir = src/tools/pgguclifetimes
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

libpq_backend_dir = $(top_builddir)/src/backend/libpq

# All files in this directory use LLVM.
CFLAGS += $(LLVM_CFLAGS)
CXXFLAGS += $(LLVM_CXXFLAGS)
override CPPFLAGS := $(LLVM_CPPFLAGS) $(CPPFLAGS)
SHLIB_LINK += $(LLVM_LIBS)

override CPPFLAGS := -lllvm -lclang -I$(libpq_backend_dir) $(CPPFLAGS)

OBJS = \
	pgguclifetimes.o \
	vector.o

all: pgguclifetimes

pgguclifetimes: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -lclang -o $@$(X)

clean distclean:
	rm -f pgguclifetimes$(X) $(OBJS)
