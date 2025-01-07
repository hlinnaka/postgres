/*-------------------------------------------------------------------------
 *
 * aio_init.c
 *    AIO - Subsystem Initialization
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio_init.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/aio_init.h"


Size
AioShmemSize(void)
{
	Size		sz = 0;

	return sz;
}

void
AioShmemInit(void)
{
}

void
pgaio_init_backend(void)
{
}

void
pgaio_postmaster_child_init_local(void)
{
}
