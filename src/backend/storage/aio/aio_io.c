/*-------------------------------------------------------------------------
 *
 * aio_io.c
 *    AIO - Low Level IO Handling
 *
 * Functions related to associating IO operations to IO Handles and IO-method
 * independent support functions for actually performing IO.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio_io.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/fd.h"
#include "utils/wait_event.h"


static void pgaio_io_before_prep(PgAioHandle *ioh);



/* --------------------------------------------------------------------------------
 * "Preparation" routines for individual IO types
 *
 * These are called by XXX place the place actually initiating an IO, to associate
 * the IO specific data with an AIO handle.
 *
 * Each of the preparation routines first needs to call
 * pgaio_io_before_prep(), then fill IO specific fields in the handle and then
 * finally call pgaio_io_prepare().
 * --------------------------------------------------------------------------------
 */

void
pgaio_io_prep_readv(PgAioHandle *ioh,
					int fd, int iovcnt, uint64 offset)
{
	pgaio_io_before_prep(ioh);

	ioh->op_data.read.fd = fd;
	ioh->op_data.read.offset = offset;
	ioh->op_data.read.iov_length = iovcnt;

	pgaio_io_prepare(ioh, PGAIO_OP_READV);
}

void
pgaio_io_prep_writev(PgAioHandle *ioh,
					 int fd, int iovcnt, uint64 offset)
{
	pgaio_io_before_prep(ioh);

	ioh->op_data.write.fd = fd;
	ioh->op_data.write.offset = offset;
	ioh->op_data.write.iov_length = iovcnt;

	pgaio_io_prepare(ioh, PGAIO_OP_WRITEV);
}



/* --------------------------------------------------------------------------------
 * Functions implementing IO handle operations that are directly related to IO
 * operations.
 * --------------------------------------------------------------------------------
 */

/*
 * Execute IO operation synchronously. This is implemented here, not in
 * method_sync.c, because other IO methods lso might use it / fall back to it.
 */
void
pgaio_io_perform_synchronously(PgAioHandle *ioh)
{
	ssize_t		result = 0;
	struct iovec *iov = &aio_ctl->iovecs[ioh->iovec_off];

	/* Perform IO. */
	switch (ioh->op)
	{
		case PGAIO_OP_READV:
			pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_READ);
			result = pg_preadv(ioh->op_data.read.fd, iov,
							   ioh->op_data.read.iov_length,
							   ioh->op_data.read.offset);
			pgstat_report_wait_end();
			break;
		case PGAIO_OP_WRITEV:
			pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
			result = pg_pwritev(ioh->op_data.write.fd, iov,
								ioh->op_data.write.iov_length,
								ioh->op_data.write.offset);
			pgstat_report_wait_end();
			break;
		case PGAIO_OP_INVALID:
			elog(ERROR, "trying to execute invalid IO operation");
	}

	ioh->result = result < 0 ? -errno : result;

	pgaio_io_process_completion(ioh, ioh->result);
}

const char *
pgaio_io_get_op_name(PgAioHandle *ioh)
{
	Assert(ioh->op >= 0 && ioh->op < PGAIO_OP_COUNT);

	switch (ioh->op)
	{
		case PGAIO_OP_INVALID:
			return "invalid";
		case PGAIO_OP_READV:
			return "read";
		case PGAIO_OP_WRITEV:
			return "write";
	}

	pg_unreachable();
}

/*
 * Helper function to be called by IO operation preparation functions, before
 * any data in the handle is set.  Mostly to centralize assertions.
 */
static void
pgaio_io_before_prep(PgAioHandle *ioh)
{
	Assert(ioh->state == AHS_HANDED_OUT);
	Assert(pgaio_io_has_subject(ioh));
}
