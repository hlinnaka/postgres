/*
 * slru_io.c
 *
 * Routines for reading and writing SLRU files during upgrade.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/slru_io.c
 */

#include "postgres_fe.h"

#include <fcntl.h>

#include "common/fe_memutils.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "port/pg_iovec.h"
#include "pg_upgrade.h"
#include "slru_io.h"

static SlruSegState *AllocSlruSegState(const char *dir);
static char *SlruFileName(SlruSegState *state, int64 segno);
static void SlruFlush(SlruSegState *state);

static SlruSegState *
AllocSlruSegState(const char *dir)
{
	SlruSegState *state = pg_malloc(sizeof(*state));

	state->dir = pstrdup(dir);
	state->fn = NULL;
	state->fd = -1;
	state->segno = -1;
	state->pageno = 0;

	return state;
}

/* similar to the backend function with the same name */
static char *
SlruFileName(SlruSegState *state, int64 segno)
{
	if (state->long_segment_names)
	{
		Assert(segno >= 0 && segno <= INT64CONST(0xFFFFFFFFFFFFFFF));
		return psprintf("%s/%015" PRIX64, state->dir, segno);
	}
	else
	{
		Assert(segno >= 0 && segno <= INT64CONST(0xFFFFFF));
		return psprintf("%s/%04X", state->dir, (unsigned int) segno);
	}
}

/*
 * Create slru reader for dir.
 *
 * Returns the malloced memory used by the all other read calls in this module.
 */
SlruSegState *
AllocSlruRead(const char *dir, bool long_segment_names)
{
	SlruSegState *state = AllocSlruSegState(dir);

	state->writing = false;
	state->long_segment_names = long_segment_names;

	return state;
}

/*
 * Open given page for reading.
 *
 * Reading can be done in random order.
 */
char *
SlruReadSwitchPageSlow(SlruSegState *state, uint64 pageno)
{
	int64		segno;
	ssize_t		bytes_read;
	off_t		offset;

	Assert(!state->writing);	/* read only mode */

	if (state->segno != -1 && pageno == state->pageno)
		return state->buf.data;

	segno = pageno / SLRU_PAGES_PER_SEGMENT;
	if (segno != state->segno)
	{
		if (state->segno != -1)
		{
			close(state->fd);
			state->fd = -1;

			pg_free(state->fn);
			state->fn = NULL;

			state->segno = -1;
		}

		/* Open new segment */
		state->fn = SlruFileName(state, segno);
		if ((state->fd = open(state->fn, O_RDONLY | PG_BINARY, 0)) < 0)
			pg_fatal("could not open file \"%s\": %m", state->fn);
	}
	state->segno = segno;

	offset = (pageno % SLRU_PAGES_PER_SEGMENT) * BLCKSZ;
	bytes_read = 0;
	while (bytes_read < BLCKSZ)
	{
		ssize_t		rc;

		rc = pg_pread(state->fd,
					  &state->buf.data + bytes_read,
					  BLCKSZ - bytes_read,
					  offset + bytes_read);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			pg_fatal("could not read file \"%s\": %m", state->fn);
		}
		if (rc == 0)
		{
			/* unexpected EOF */
			pg_log(PG_WARNING, "unexpected EOF reading file \"%s\" at offset %zd, reading as zeros", state->fn,
				   offset + bytes_read);
			memset(&state->buf.data + bytes_read, 0, BLCKSZ - bytes_read);
			break;
		}
		bytes_read += rc;
	}
	state->pageno = pageno;

	return state->buf.data;
}

/*
 * Frees the malloced reader.
 */
void
FreeSlruRead(SlruSegState *state)
{
	Assert(!state->writing);	/* read only mode */

	if (state->fd != -1)
		close(state->fd);
	pg_free(state);
}

/*
 * Create slru writer for dir.
 *
 * Returns the malloced memory used by the all other write calls in this module.
 */
SlruSegState *
AllocSlruWrite(const char *dir, bool long_segment_names)
{
	SlruSegState *state = AllocSlruSegState(dir);

	state->writing = true;
	state->long_segment_names = long_segment_names;

	return state;
}

/*
 * Open the given page for writing.
 *
 * NOTE: This uses O_EXCL when stepping to a new segment, so this assumes that
 * each segment is written in full before moving on to next one.  This
 * limitation would be easy to lift if needed, but it fits the usage pattern of
 * current callers.
 */
char *
SlruWriteSwitchPageSlow(SlruSegState *state, uint64 pageno)
{
	int64		segno;
	off_t		offset;

	if (state->segno != -1 && pageno == state->pageno)
		return state->buf.data;

	segno = pageno / SLRU_PAGES_PER_SEGMENT;
	offset = (pageno % SLRU_PAGES_PER_SEGMENT) * BLCKSZ;

	SlruFlush(state);
	memset(state->buf.data, 0, BLCKSZ);

	if (segno != state->segno)
	{
		if (state->segno != -1)
		{
			close(state->fd);
			state->fd = -1;

			pg_free(state->fn);
			state->fn = NULL;

			state->segno = -1;
		}

		/* Create the segment */
		state->fn = SlruFileName(state, segno);
		if ((state->fd = open(state->fn, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
							  pg_file_create_mode)) < 0)
		{
			pg_fatal("could not create file \"%s\": %m", state->fn);
		}

		state->segno = segno;

		if (offset > 0)
		{
			if (pg_pwrite_zeros(state->fd, offset, 0) < 0)
				pg_fatal("could not write file \"%s\": %m", state->fn);
		}
	}

	state->pageno = pageno;

	return state->buf.data;
}

static void
SlruFlush(SlruSegState *state)
{
	struct iovec iovec = {
		.iov_base = &state->buf,
		.iov_len = BLCKSZ,
	};
	off_t		offset;

	if (state->segno == -1)
		return;

	offset = (state->pageno % SLRU_PAGES_PER_SEGMENT) * BLCKSZ;

	if (pg_pwritev_with_retry(state->fd, &iovec, 1, offset) < 0)
		pg_fatal("could not write file \"%s\": %m", state->fn);
}

/*
 * Frees the malloced writer.
 */
void
FreeSlruWrite(SlruSegState *state)
{
	Assert(state->writing);

	SlruFlush(state);

	if (state->fd != -1)
		close(state->fd);
	pg_free(state);
}
