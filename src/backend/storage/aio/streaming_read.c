/*-------------------------------------------------------------------------
 *
 * streaming_read.c
 *	  Mechanism for buffer access with look-ahead
 *
 * Portions Copyright (c) 2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * Code that needs to access relation data typically pins blocks one at a
 * time, often in a predictable order that might be sequential or data-driven.
 * Calling the simple ReadBuffer() function for each block is inefficient,
 * because blocks that are not yet in the buffer pool require I/O operations
 * that are small and might stall waiting for storage.  This mechanism looks
 * into the future and calls StartReadBuffers() and WaitReadBuffers() to read
 * neighboring blocks together and ahead of time, with an adaptive look-ahead
 * distance.
 *
 * A user-provided callback generates a stream of block numbers that is used
 * to form reads of up to size buffer_io_size, by attempting to merge them
 * with a pending read.  When that isn't possible, the existing pending read
 * is sent to StartReadBuffers() so that a new one can begin to form.
 *
 * The algorithm for controlling the look-ahead distance tries to classify the
 * stream into three ideal behaviors:
 *
 * A) No I/O is necessary, because the requested blocks are fully cached
 * already.  There is no benefit to looking ahead more than one block, so
 * distance is 1.  This is the default initial assumption.
 *
 * B) I/O is necessary, but fadvise is undesirable because the access is
 * sequential, or impossible because direct I/O is enabled or the system
 * doesn't support advice.  There is no benefit in looking ahead more than
 * buffer_io_size (the GUC controlling physical read size), because in this
 * case only goal is larger read system calls.  Looking further ahead would
 * pin many buffers and perform speculative work looking ahead for no benefit.
 *
 * C) I/O is necesssary, it appears random, and this system supports fadvise.
 * We'll look further ahead in order to reach the configured level of I/O
 * concurrency.
 *
 * The distance increases rapidly and decays slowly, so that it moves towards
 * those levels as different I/O patterns are discovered.  For example, a
 * sequential scan of fully cached data doesn't bother looking ahead, but a
 * sequential scan that hits a region of uncached blocks will start issuing
 * increasingly wide read calls until it plateaus at buffer_io_size.
 *
 * The main data structure is a circular queue of buffers of size
 * max_pinned_buffers, ready to be returned by streaming_read_buffer_next().
 * Each buffer also has an optional variable sized object that is passed from
 * the callback to the consumer of buffers.  A third array records whether
 * WaitReadBuffers() must be called before returning the buffer, and if so,
 * points to the relevant ReadBuffersOperation object.
 *
 * For example, if the callback return block numbers 10, 42, 43, 60 in
 * successive calls, then these data structures might appear as follows:
 *
 *                          buffers buf/data buf/io       ios
 *
 *                          +----+  +-----+  +---+        +--------+
 *                          |    |  |     |  |   |  +---->| 42..44 |
 *                          +----+  +-----+  +---+  |     +--------+
 *   oldest_buffer_index -> | 10 |  |  ?  |  |   |  | +-->| 60..60 |
 *                          +----+  +-----+  +---+  | |   +--------+
 *                          | 42 |  |  ?  |  | 0 +--+ |   |        |
 *                          +----+  +-----+  +---+    |   +--------+
 *                          | 43 |  |  ?  |  |   |    |   |        |
 *                          +----+  +-----+  +---+    |   +--------+
 *                          | 44 |  |  ?  |  |   |    |   |        |
 *                          +----+  +-----+  +---+    |   +--------+
 *                          | 60 |  |  ?  |  | 1 +----+
 *                          +----+  +-----+  +---+
 *     next_buffer_index -> |    |  |     |  |   |
 *                          +----+  +-----+  +---+
 *
 * In the example, 5 buffers are pinned, and the next buffer to be streamed to
 * the client is block 10.  Block 10 was a hit and has no associated I/O, but
 * the range 42..44 requires an I/O wait before its buffers are returned, as
 * does block 60.
 *
 * IDENTIFICATION
 *	  src/backend/storage/storage/aio/streaming_read.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_tablespace.h"
#include "miscadmin.h"
#include "storage/streaming_read.h"
#include "utils/rel.h"
#include "utils/spccache.h"

/*
 * Streaming read object.
 */
struct StreamingRead
{
	int16		max_ios;
	int16		ios_in_progress;
	int16		max_pinned_buffers;
	int16		pinned_buffers;
	int16		distance;
	bool		started;
	bool		finished;
	bool		advice_enabled;

	/*
	 * The callback that will tell us which block numbers to read, and an
	 * opaque pointer that will be pass to it for its own purposes.
	 */
	StreamingReadBufferCB callback;
	void	   *callback_private_data;

	/* The relation we will read. */
	BufferAccessStrategy strategy;
	BufferManagerRelation bmr;
	ForkNumber	forknum;

	/* Sometimes we need to buffer one block for flow control. */
	BlockNumber unget_blocknum;
	void	   *unget_per_buffer_data;

	/* Next expected block, for detecting sequential access. */
	BlockNumber seq_blocknum;

	/* The read operation we are currently preparing. */
	BlockNumber pending_read_blocknum;
	int16		pending_read_nblocks;

	/* Space for buffers and optional per-buffer private data. */
	Buffer	   *buffers;
	size_t		per_buffer_data_size;
	void	   *per_buffer_data;
	int16	   *buffer_io_indexes;

	/* Read operations that have been started by not waited for yet. */
	ReadBuffersOperation *ios;
	int16		next_io_index;

	/* Head and tail of the circular queue of buffers. */
	int16		oldest_buffer_index;	/* Next pinned buffer to return */
	int16		next_buffer_index;	/* Index of next buffer to pin */
};

/*
 * Return a pointer to the per-buffer data by index.
 */
static void *
get_per_buffer_data(StreamingRead *stream, int16 buffer_index)
{
	return (char *) stream->per_buffer_data +
		stream->per_buffer_data_size * buffer_index;
}

/*
 * Ask the callback which block it would like us to read next, with a small
 * buffer in front to allow streaming_unget_block() to work.
 */
static BlockNumber
streaming_read_get_block(StreamingRead *stream, void *per_buffer_data)
{
	BlockNumber result;

	if (unlikely(stream->unget_blocknum != InvalidBlockNumber))
	{
		/*
		 * If we had to unget a block, now it is time to return that one
		 * again.
		 */
		result = stream->unget_blocknum;
		stream->unget_blocknum = InvalidBlockNumber;

		/*
		 * The same per_buffer_data element must have been used, and still
		 * contains whatever data the callback wrote into it.  So we just
		 * sanity-check that we were called with the value that
		 * streaming_unget_block() pushed back.
		 */
		Assert(per_buffer_data == stream->unget_per_buffer_data);
	}
	else
	{
		/* Use the installed callback directly. */
		result = stream->callback(stream,
								  stream->callback_private_data,
								  per_buffer_data);
	}

	return result;
}

/*
 * In order to deal with short reads in StartReadBuffers(), we sometimes need
 * to defer handling of a block until later.  This *must* be called with the
 * last value returned by streaming_get_block().
 */
static void
streaming_read_unget_block(StreamingRead *stream, BlockNumber blocknum, void *per_buffer_data)
{
	Assert(stream->unget_blocknum == InvalidBlockNumber);
	stream->unget_blocknum = blocknum;
	stream->unget_per_buffer_data = per_buffer_data;
}

static void
streaming_read_start_pending_read(StreamingRead *stream)
{
	bool		need_wait;
	int			nblocks;
	int16		io_index;
	int16		overflow;
	int			flags;

	/* This should only be called with a pending read. */
	Assert(stream->pending_read_nblocks > 0);
	Assert(stream->pending_read_nblocks <= buffer_io_size);

	/* We had better not exceed the pin limit by starting this read. */
	Assert(stream->pinned_buffers + stream->pending_read_nblocks <=
		   stream->max_pinned_buffers);

	/* We had better not be overwriting an existing pinned buffer. */
	if (stream->pinned_buffers > 0)
		Assert(stream->next_buffer_index != stream->oldest_buffer_index);
	else
		Assert(stream->next_buffer_index == stream->oldest_buffer_index);

	/*
	 * If advice hasn't been suppressed, and this system supports it, this
	 * isn't a strictly sequential pattern, then we'll issue advice.
	 */
	if (stream->advice_enabled &&
		stream->started &&
		stream->pending_read_blocknum != stream->seq_blocknum)
		flags = READ_BUFFERS_ISSUE_ADVICE;
	else
		flags = 0;

	/* Suppress advice on the first call, because it's too late to benefit. */
	if (!stream->started)
		stream->started = true;

	/* We say how many blocks we want to read, but may be smaller on return. */
	nblocks = stream->pending_read_nblocks;
	need_wait =
		StartReadBuffers(stream->bmr,
						 &stream->buffers[stream->next_buffer_index],
						 stream->forknum,
						 stream->pending_read_blocknum,
						 &nblocks,
						 stream->strategy,
						 flags,
						 &stream->ios[stream->next_io_index]);
	stream->pinned_buffers += nblocks;

	/* Remember whether we need to wait before returning this buffer. */
	if (!need_wait)
	{
		io_index = -1;

		/* Look-ahead distance decays, no I/O necessary (behavior A). */
		if (stream->distance > 1)
			stream->distance--;
	}
	else
	{
		/*
		 * Remember to call WaitReadBuffers() before returning head buffer.
		 * Look-ahead distance will be adjusted after waiting.
		 */
		io_index = stream->next_io_index;
		if (++stream->next_io_index == stream->max_ios)
			stream->next_io_index = 0;

		Assert(stream->ios_in_progress < stream->max_ios);
		stream->ios_in_progress++;
	}

	/* Set up the pointer to the I/O for the head buffer, if there is one. */
	stream->buffer_io_indexes[stream->next_buffer_index] = io_index;

	/*
	 * We gave a contiguous range of buffer space to StartReadBuffers(), but
	 * we want it to wrap around at max_pinned_buffers.  Move values that
	 * overflowed into the extra space.  At the same time, put -1 in the I/O
	 * slots for the rest of the buffers to indicate no I/O.  They are covered
	 * by the head buffer's I/O, if there is one.  We avoid a % operator.
	 */
	overflow = (stream->next_buffer_index + nblocks) - stream->max_pinned_buffers;
	if (overflow > 0)
	{
		memmove(&stream->buffers[0],
				&stream->buffers[stream->max_pinned_buffers],
				sizeof(stream->buffers[0]) * overflow);
		for (int i = 0; i < overflow; ++i)
			stream->buffer_io_indexes[i] = -1;
		for (int i = 1; i < nblocks - overflow; ++i)
			stream->buffer_io_indexes[stream->next_buffer_index + i] = -1;
	}
	else
	{
		for (int i = 1; i < nblocks; ++i)
			stream->buffer_io_indexes[stream->next_buffer_index + i] = -1;
	}

	/*
	 * Remember where the next block would be after that, so we can detect
	 * sequential access next time and suppress advice.
	 */
	stream->seq_blocknum = stream->pending_read_blocknum + nblocks;

	/* Compute location of start of next read, without using % operator. */
	stream->next_buffer_index += nblocks;
	if (stream->next_buffer_index >= stream->max_pinned_buffers)
		stream->next_buffer_index -= stream->max_pinned_buffers;
	Assert(stream->next_buffer_index >= 0);
	Assert(stream->next_buffer_index < stream->max_pinned_buffers);

	/* Adjust the pending read to cover the remaining portion, if any. */
	stream->pending_read_blocknum += nblocks;
	stream->pending_read_nblocks -= nblocks;
}

static void
streaming_read_look_ahead(StreamingRead *stream)
{
	while (!stream->finished &&
		   stream->ios_in_progress < stream->max_ios &&
		   stream->pinned_buffers + stream->pending_read_nblocks < stream->distance)
	{
		BlockNumber blocknum;
		int16		buffer_index;
		void	   *per_buffer_data;

		if (stream->pending_read_nblocks == buffer_io_size)
		{
			streaming_read_start_pending_read(stream);
			continue;
		}

		/*
		 * See which block the callback wants next in the stream.  We need to
		 * compute the index of the Nth block of the pending read including
		 * wrap-around, but we don't want to use the expensive % operator.
		 */
		buffer_index = stream->next_buffer_index + stream->pending_read_nblocks;
		if (buffer_index > stream->max_pinned_buffers)
			buffer_index -= stream->max_pinned_buffers;
		per_buffer_data = get_per_buffer_data(stream, buffer_index);
		blocknum = streaming_read_get_block(stream, per_buffer_data);
		if (blocknum == InvalidBlockNumber)
		{
			stream->finished = true;
			continue;
		}

		/* Can we merge it with the pending read? */
		if (stream->pending_read_nblocks > 0 &&
			stream->pending_read_blocknum + stream->pending_read_nblocks == blocknum)
		{
			stream->pending_read_nblocks++;
			continue;
		}

		/* We have to start the pending read before we can build another. */
		if (stream->pending_read_nblocks > 0)
		{
			streaming_read_start_pending_read(stream);
			if (stream->ios_in_progress == stream->max_ios)
			{
				/* And we've hit the limit.  Rewind, and stop here. */
				streaming_read_unget_block(stream, blocknum, per_buffer_data);
				return;
			}
		}

		/* This is the start of a new pending read. */
		stream->pending_read_blocknum = blocknum;
		stream->pending_read_nblocks = 1;
	}

	/*
	 * Normally we don't start the pending read just because we've hit a
	 * limit, preferring to give it another chance to grow to a larger size
	 * once more buffers have been consumed.  However, in cases where that
	 * can't possibly happen, we might as well start the read immediately.
	 */
	if (((stream->pending_read_nblocks > 0 && stream->finished) ||
		 (stream->pending_read_nblocks == stream->distance)) &&
		stream->ios_in_progress < stream->max_ios)
		streaming_read_start_pending_read(stream);
}

/*
 * Create a new streaming read object that can be used to perform the
 * equivalent of a series of ReadBuffer() calls for one fork of one relation.
 * Internally, it generates larger vectored reads where possible by looking
 * ahead.  The callback should return block numbers or InvalidBlockNumber to
 * signal end-of-stream, and if per_buffer_data_size is non-zero, it may also
 * write extra data for each block into the space provided to it.  It will
 * also receive callback_private_data for its own purposes.
 */
StreamingRead *
streaming_read_buffer_begin(int flags,
							BufferAccessStrategy strategy,
							BufferManagerRelation bmr,
							ForkNumber forknum,
							StreamingReadBufferCB callback,
							void *callback_private_data,
							size_t per_buffer_data_size)
{
	StreamingRead *stream;
	int16		max_ios;
	uint32		max_pinned_buffers;
	Oid			tablespace_id;

	/*
	 * Make sure our bmr's smgr and persistent are populated.  The caller
	 * asserts that the storage manager will remain valid.
	 */
	if (!bmr.smgr)
	{
		bmr.smgr = RelationGetSmgr(bmr.rel);
		bmr.relpersistence = bmr.rel->rd_rel->relpersistence;
	}

	/*
	 * Decide how many assumed I/Os we will allow to run concurrently.  That
	 * is, advice to the kernel to tell it that we will soon read.  This
	 * number also affects how far we look ahead for opportunities to start
	 * more I/Os.
	 */
	tablespace_id = bmr.smgr->smgr_rlocator.locator.spcOid;
	if (!OidIsValid(MyDatabaseId) ||
		(bmr.rel && IsCatalogRelation(bmr.rel)) ||
		IsCatalogRelationOid(bmr.smgr->smgr_rlocator.locator.relNumber))
	{
		/*
		 * Avoid circularity while trying to look up tablespace settings or
		 * before spccache.c is ready.
		 */
		max_ios = effective_io_concurrency;
	}
	else if (flags & STREAMING_READ_MAINTENANCE)
		max_ios = get_tablespace_maintenance_io_concurrency(tablespace_id);
	else
		max_ios = get_tablespace_io_concurrency(tablespace_id);

	/*
	 * Choose a maximum number of buffers we're prepared to pin.  We try to
	 * pin fewer if we can, though.  We clamp it to at least buffer_io_size so
	 * that we can have a chance to build up a full sized read, even when
	 * max_ios is zero.
	 */
	max_pinned_buffers = Max(max_ios * 4, buffer_io_size);

	/* Don't allow this backend to pin more than its share of buffers. */
	if (SmgrIsTemp(bmr.smgr))
		LimitAdditionalLocalPins(&max_pinned_buffers);
	else
		LimitAdditionalPins(&max_pinned_buffers);
	Assert(max_pinned_buffers > 0);

	stream = (StreamingRead *) palloc0(sizeof(StreamingRead));

#ifdef USE_PREFETCH

	/*
	 * This system supports prefetching advice.  We can use it as long as
	 * direct I/O isn't enabled, the caller hasn't promised sequential access
	 * (overriding our detection heuristics), and max_ios hasn't been set to
	 * zero.
	 */
	if ((io_direct_flags & IO_DIRECT_DATA) == 0 &&
		(flags & STREAMING_READ_SEQUENTIAL) == 0 &&
		max_ios > 0)
		stream->advice_enabled = true;
#endif

	/*
	 * For now, max_ios = 0 is interpreted as max_ios = 1 with advice disabled
	 * above.  If we had real asynchronous I/O we might need a slightly
	 * different definition.
	 */
	if (max_ios == 0)
		max_ios = 1;

	stream->max_ios = max_ios;
	stream->per_buffer_data_size = per_buffer_data_size;
	stream->max_pinned_buffers = max_pinned_buffers;
	stream->strategy = strategy;

	stream->bmr = bmr;
	stream->forknum = forknum;
	stream->callback = callback;
	stream->callback_private_data = callback_private_data;

	stream->unget_blocknum = InvalidBlockNumber;

	/*
	 * Skip the initial ramp-up phase if the caller says we're going to be
	 * reading the whole relation.  This way we start out doing full-sized
	 * reads.
	 */
	if (flags & STREAMING_READ_FULL)
		stream->distance = stream->max_pinned_buffers;
	else
		stream->distance = 1;

	/*
	 * Space for the buffers we pin.  Though we never pin more than
	 * max_pinned_buffers, we want to be able to assume that all the buffers
	 * for a single read are contiguous (i.e. don't wrap around halfway
	 * through), so we let the final one run past that position temporarily by
	 * allocating an extra buffer_io_size - 1 elements.
	 */
	stream->buffers = palloc((max_pinned_buffers + buffer_io_size - 1) *
							 sizeof(stream->buffers[0]));

	/* Space for per-buffer data, if configured. */
	if (per_buffer_data_size)
		stream->per_buffer_data =
			palloc(per_buffer_data_size * (max_pinned_buffers +
										   buffer_io_size - 1));

	/* Space for the IOs that we might run. */
	stream->buffer_io_indexes = palloc(max_pinned_buffers * sizeof(stream->buffer_io_indexes[0]));
	stream->ios = palloc(max_ios * sizeof(ReadBuffersOperation));

	return stream;
}

/*
 * Pull one pinned buffer out of a stream created with
 * streaming_read_buffer_begin().  Each call returns successive blocks in the
 * order specified by the callback.  If per_buffer_data_size was set to a
 * non-zero size, *per_buffer_data receives a pointer to the extra per-buffer
 * data that the callback had a chance to populate.  When the stream runs out
 * of data, InvalidBuffer is returned.  The caller may decide to end the
 * stream early at any time by calling streaming_read_end().
 */
Buffer
streaming_read_buffer_next(StreamingRead *stream, void **per_buffer_data)
{
	Buffer		buffer;
	int16		io_index;
	int16		oldest_buffer_index;

	if (unlikely(stream->pinned_buffers == 0))
	{
		Assert(stream->oldest_buffer_index == stream->next_buffer_index);

		if (stream->finished)
			return InvalidBuffer;

		/*
		 * The usual order of operations is that we look ahead at the bottom
		 * of this function after potentially finishing an I/O and making
		 * space for more, but we need a special case to prime the stream when
		 * we're getting started.
		 */
		Assert(!stream->started);
		streaming_read_look_ahead(stream);
		if (stream->pinned_buffers == 0)
			return InvalidBuffer;
	}

	/* Grab the oldest pinned buffer and associated per-buffer data. */
	oldest_buffer_index = stream->oldest_buffer_index;
	Assert(oldest_buffer_index >= 0 &&
		   oldest_buffer_index < stream->max_pinned_buffers);
	buffer = stream->buffers[oldest_buffer_index];
	if (per_buffer_data)
		*per_buffer_data = get_per_buffer_data(stream, oldest_buffer_index);

	Assert(BufferIsValid(buffer));

	/* Do we have to wait for an associated I/O first? */
	io_index = stream->buffer_io_indexes[oldest_buffer_index];
	Assert(io_index >= -1 && io_index < stream->max_ios);
	if (io_index >= 0)
	{
		int			distance;

		/* Sanity check that we still agree on the buffers. */
		Assert(stream->ios[io_index].buffers == &stream->buffers[oldest_buffer_index]);

		WaitReadBuffers(&stream->ios[io_index]);

		Assert(stream->ios_in_progress > 0);
		stream->ios_in_progress--;

		if (stream->ios[io_index].flags & READ_BUFFERS_ISSUE_ADVICE)
		{
			/* Distance ramps up fast (behavior C). */
			distance = stream->distance * 2;
			distance = Min(distance, stream->max_pinned_buffers);
			stream->distance = distance;
		}
		else
		{
			/* No advice; move towards full I/O size (behavior B). */
			if (stream->distance > buffer_io_size)
			{
				stream->distance--;
			}
			else
			{
				distance = stream->distance * 2;
				distance = Min(distance, buffer_io_size);
				distance = Min(distance, stream->max_pinned_buffers);
				stream->distance = distance;
			}
		}
	}

	/* Advance the oldest buffer, but clobber it first for debugging. */
#ifdef USE_ASSERT_CHECKING
	stream->buffers[oldest_buffer_index] = InvalidBuffer;
	stream->buffer_io_indexes[oldest_buffer_index] = -1;
	if (stream->per_buffer_data)
		memset(get_per_buffer_data(stream, oldest_buffer_index),
			   0xff,
			   stream->per_buffer_data_size);
#endif
	if (++stream->oldest_buffer_index == stream->max_pinned_buffers)
		stream->oldest_buffer_index = 0;

	/* We are transferring ownership of the pin to the caller. */
	Assert(stream->pinned_buffers > 0);
	stream->pinned_buffers--;

	/*
	 * When distance is minimal, we finish up with no queued buffers.  As a
	 * micro-optimization, we can then reset our circular queues, so that
	 * all-cached streams re-use the same elements instead of rotating through
	 * memory.
	 */
	if (stream->pinned_buffers == 0)
	{
		Assert(stream->oldest_buffer_index == stream->next_buffer_index);
		stream->oldest_buffer_index = 0;
		stream->next_buffer_index = 0;
		stream->next_io_index = 0;
	}

	/* Prepare for the next call. */
	streaming_read_look_ahead(stream);

	return buffer;
}

/*
 * Finish streaming blocks and release all resources.
 */
void
streaming_read_buffer_end(StreamingRead *stream)
{
	Buffer		buffer;

	/* Stop looking ahead. */
	stream->finished = true;

	/* Unpin anything that wasn't consumed. */
	while ((buffer = streaming_read_buffer_next(stream, NULL)) != InvalidBuffer)
		ReleaseBuffer(buffer);

	Assert(stream->pinned_buffers == 0);
	Assert(stream->ios_in_progress == 0);

	/* Release memory. */
	pfree(stream->buffers);
	if (stream->per_buffer_data)
		pfree(stream->per_buffer_data);
	pfree(stream->ios);

	pfree(stream);
}
