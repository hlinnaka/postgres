/*-------------------------------------------------------------------------
 *
 * test_shmem.c
 *		Helpers to test shmem allocation routines
 *
 *  XXX This module provides interface functions for C functionality to SQL, to
 * make it possible to test AIO related behavior in a targeted way from SQL.
 * It'd not generally be safe to export these functions to SQL, but for a test
 * that's fine.
 *
 * Copyright (c) 2020-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_shmem/test_shmem.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"


PG_MODULE_MAGIC;

typedef struct TestShmemData
{
	int			value;
	bool		initialized;
	int			attach_count;
} TestShmemData;

static TestShmemData *TestShmem;

static bool attached_or_initialized = false;

/* Saved hook values */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void test_shmem_request(void);
static void test_shmem_startup(void);

void
_PG_init(void)
{
	elog(LOG, "test_shmem module's _PG_init called");

	if (process_shared_preload_libraries_in_progress)
	{
		prev_shmem_request_hook = shmem_request_hook;
		shmem_request_hook = test_shmem_request;
		prev_shmem_startup_hook = shmem_startup_hook;
		shmem_startup_hook = test_shmem_startup;
	}
	else
	{
		test_shmem_startup();
	}
}

static void
test_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
}

static void
test_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	TestShmem = ShmemInitStruct("test_shmem",
								sizeof(TestShmemData) + 10 * 1024,
								&found);
	if (!found)
	{
		Assert(!TestShmem->initialized);
		TestShmem->initialized = true;
	}
	else
	{
		Assert(TestShmem->initialized);
		TestShmem->attach_count++;
	}
	attached_or_initialized = true;

	LWLockRelease(AddinShmemInitLock);
}


PG_FUNCTION_INFO_V1(get_test_shmem_attach_count);
Datum
get_test_shmem_attach_count(PG_FUNCTION_ARGS)
{
	if (!attached_or_initialized)
		elog(ERROR, "shmem area not attached or initialized in this process");
	if (!TestShmem->initialized)
		elog(ERROR, "shmem area not yet initialized");
	PG_RETURN_INT32(TestShmem->attach_count);
}
