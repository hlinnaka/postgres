/*-------------------------------------------------------------------------
 *
 * aio.c
 *    AIO - Core Logic
 *
 * For documentation about how AIO works on a higher level, including a
 * schematic example, see README.md.
 *
 *
 * AIO is a complicated subsystem. To keep things navigable it is split across
 * a number of files:
 *
 * - aio.c - core AIO state handling
 *
 * - aio_init.c - initialization
 *
 * - aio_io.c - dealing with actual IO, including executing IOs synchronously
 *
 * - aio_subject.c - functionality related to executing IO for different
 *   subjects
 *
 * - method_*.c - different ways of executing AIO
 *
 * - read_stream.c - helper for accessing buffered relation data with
 *	 look-ahead
 *
 * - README.md - higher-level overview over AIO
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/bufmgr.h"
#include "utils/resowner.h"
#include "utils/wait_event_types.h"

#ifdef USE_INJECTION_POINTS
#include "utils/injection_point.h"
#endif


static inline void pgaio_io_update_state(PgAioHandle *ioh, PgAioHandleState new_state);
static void pgaio_io_reclaim(PgAioHandle *ioh);
static void pgaio_io_resowner_register(PgAioHandle *ioh);
static void pgaio_io_wait_for_free(void);
static PgAioHandle *pgaio_io_from_ref(PgAioHandleRef *ior, uint64 *ref_generation);

static void pgaio_bounce_buffer_wait_for_free(void);



/* Options for io_method. */
const struct config_enum_entry io_method_options[] = {
	{"sync", IOMETHOD_SYNC, false},
	{"worker", IOMETHOD_WORKER, false},
#ifdef USE_LIBURING
	{"io_uring", IOMETHOD_IO_URING, false},
#endif
	{NULL, 0, false}
};

int			io_method = DEFAULT_IO_METHOD;
int			io_max_concurrency = -1;
int			io_bounce_buffers = -1;


/* global control for AIO */
PgAioCtl   *aio_ctl;

/* current backend's per-backend state */
PgAioPerBackend *my_aio;


static const IoMethodOps *pgaio_ops_table[] = {
	[IOMETHOD_SYNC] = &pgaio_sync_ops,
	[IOMETHOD_WORKER] = &pgaio_worker_ops,
#ifdef USE_LIBURING
	[IOMETHOD_IO_URING] = &pgaio_uring_ops,
#endif
};


const IoMethodOps *pgaio_impl;


#ifdef USE_INJECTION_POINTS
static PgAioHandle *inj_cur_handle;
#endif



/* --------------------------------------------------------------------------------
 * "Core" IO Api
 * --------------------------------------------------------------------------------
 */

/*
 * Acquire an AioHandle, waiting for IO completion if necessary.
 *
 * Each backend can only have one AIO handle that that has been "handed out"
 * to code, but not yet staged or released. This restriction is necessary
 * to ensure that it is possible for code to wait for an unused handle by
 * waiting for in-flight IO to complete. There is a limited number of handles
 * in each backend, if multiple handles could be handed out without being
 * submitted, waiting for all in-flight IO to complete would not guarantee
 * that handles free up.
 *
 * It is cheap to acquire an IO handle, unless all handles are in use. In that
 * case this function waits for the oldest IO to complete. In case that is not
 * desirable, see pgaio_io_get_nb().
 *
 * If a handle was acquired but then does not turn out to be needed,
 * e.g. because pgaio_io_get() is called before starting an IO in a critical
 * section, the handle needs to be released with pgaio_io_release().
 *
 *
 * To react to the completion of the IO as soon as it is known to have
 * completed, callbacks can be registered with pgaio_io_add_shared_cb().
 *
 * To actually execute IO using the returned handle, the pgaio_io_prep_*()
 * family of functions is used. In many cases the pgaio_io_prep_*() call will
 * not be done directly by code that acquired the handle, but by lower level
 * code that gets passed the handle. E.g. if code in bufmgr.c wants to perform
 * AIO, it typically will pass the handle to smgr., which will pass it on to
 * md.c, on to fd.c, which then finally calls pgaio_io_prep_*().  This
 * forwarding allows the various layers to react to the IO's completion by
 * registering callbacks. These callbacks in turn can translate a lower
 * layer's result into a result understandable by a higher layer.
 *
 * Once pgaio_io_prep_*() is called, the IO may be in the process of being
 * executed and might even complete before the functions return. That is,
 * however, not guaranteed, to allow IO submission to be batched. To guarantee
 * IO submission pgaio_submit_staged() needs to be called.
 *
 * After pgaio_io_prep_*() the AioHandle is "consumed" and may not be
 * referenced by the IO issuing code. To e.g. wait for IO, references to the
 * IO can be established with pgaio_io_get_ref() *before* pgaio_io_prep_*() is
 * called.  pgaio_io_ref_wait() can be used to wait for the IO to complete.
 *
 *
 * To know if the IO [partially] succeeded or failed, a PgAioReturn * can be
 * passed to pgaio_io_get(). Once the issuing backend has called
 * pgaio_io_ref_wait(), the PgAioReturn contains information about whether the
 * operation succeeded and details about the first failure, if any. The error
 * can be raised / logged with pgaio_result_log().
 *
 * The lifetime of the memory pointed to be *ret needs to be at least as long
 * as the passed in resowner. If the resowner releases resources before the IO
 * completes, the reference to *ret will be cleared.
 */
PgAioHandle *
pgaio_io_get(struct ResourceOwnerData *resowner, PgAioReturn *ret)
{
	PgAioHandle *h;

	while (true)
	{
		h = pgaio_io_get_nb(resowner, ret);

		if (h != NULL)
			return h;

		/*
		 * Evidently all handles by this backend are in use. Just wait for
		 * some to complete.
		 */
		pgaio_io_wait_for_free();
	}
}

/*
 * Acquire an AioHandle, returning NULL if no handles are free.
 *
 * See pgaio_io_get(). The only difference is that this function will return
 * NULL if there are no idle handles, instead of blocking.
 */
PgAioHandle *
pgaio_io_get_nb(struct ResourceOwnerData *resowner, PgAioReturn *ret)
{
	if (my_aio->num_staged_ios >= PGAIO_SUBMIT_BATCH_SIZE)
	{
		Assert(my_aio->num_staged_ios == PGAIO_SUBMIT_BATCH_SIZE);
		pgaio_submit_staged();
	}

	if (my_aio->handed_out_io)
	{
		ereport(ERROR,
				errmsg("API violation: Only one IO can be handed out"));
	}

	if (!dclist_is_empty(&my_aio->idle_ios))
	{
		dlist_node *ion = dclist_pop_head_node(&my_aio->idle_ios);
		PgAioHandle *ioh = dclist_container(PgAioHandle, node, ion);

		Assert(ioh->state == AHS_IDLE);
		Assert(ioh->owner_procno == MyProcNumber);

		pgaio_io_update_state(ioh, AHS_HANDED_OUT);
		my_aio->handed_out_io = ioh;

		if (resowner)
			pgaio_io_resowner_register(ioh);

		if (ret)
		{
			ioh->report_return = ret;
			ret->result.status = ARS_UNKNOWN;
		}

		return ioh;
	}

	return NULL;
}

/*
 * Release IO handle that turned out to not be required.
 *
 * See pgaio_io_get() for more details.
 */
void
pgaio_io_release(PgAioHandle *ioh)
{
	if (ioh == my_aio->handed_out_io)
	{
		Assert(ioh->state == AHS_HANDED_OUT);
		Assert(ioh->resowner);

		my_aio->handed_out_io = NULL;
		pgaio_io_reclaim(ioh);
	}
	else
	{
		elog(ERROR, "release in unexpected state");
	}
}

/*
 * Finish building an IO request.  Once a request has been staged, there's no
 * going back; the IO subsystem will attempt to perform the IO. If the IO
 * succeeds the completion callbacks will be called; on error, the error
 * callbacks.
 *
 * This may add the IO to the current batch, or execute the request
 * synchronously.
 */
void
pgaio_io_stage(PgAioHandle *ioh)
{
	bool		needs_synchronous;

	/* allow a new IO to be staged */
	my_aio->handed_out_io = NULL;

	pgaio_io_update_state(ioh, AHS_PREPARED);

	needs_synchronous = pgaio_io_needs_synchronous_execution(ioh);

	elog(DEBUG3, "io:%d: staged %s, executed synchronously: %d",
		 pgaio_io_get_id(ioh), pgaio_io_get_op_name(ioh),
		 needs_synchronous);

	if (!needs_synchronous)
	{
		my_aio->staged_ios[my_aio->num_staged_ios++] = ioh;
		Assert(my_aio->num_staged_ios <= PGAIO_SUBMIT_BATCH_SIZE);
	}
	else
	{
		pgaio_io_prepare_submit(ioh);
		pgaio_io_perform_synchronously(ioh);
	}
}

/*
 * Release IO handle during resource owner cleanup.
 */
void
pgaio_io_release_resowner(dlist_node *ioh_node, bool on_error)
{
	PgAioHandle *ioh = dlist_container(PgAioHandle, resowner_node, ioh_node);

	Assert(ioh->resowner);

	ResourceOwnerForgetAioHandle(ioh->resowner, &ioh->resowner_node);
	ioh->resowner = NULL;

	switch (ioh->state)
	{
		case AHS_IDLE:
			elog(ERROR, "unexpected");
			break;
		case AHS_HANDED_OUT:
			Assert(ioh == my_aio->handed_out_io || my_aio->handed_out_io == NULL);

			if (ioh == my_aio->handed_out_io)
			{
				my_aio->handed_out_io = NULL;
				if (!on_error)
					elog(WARNING, "leaked AIO handle");
			}

			pgaio_io_reclaim(ioh);
			break;
		case AHS_PREPARING:
		case AHS_PREPARED:
			/* XXX: Should we warn about this when is_commit? */
			pgaio_submit_staged();
			break;
		case AHS_IN_FLIGHT:
		case AHS_REAPED:
		case AHS_COMPLETED_SHARED:
			/* this is expected to happen */
			break;
		case AHS_COMPLETED_LOCAL:
			/* XXX: unclear if this ought to be possible? */
			pgaio_io_reclaim(ioh);
			break;
	}

	/*
	 * Need to unregister the reporting of the IO's result, the memory it's
	 * referencing likely has gone away.
	 */
	if (ioh->report_return)
		ioh->report_return = NULL;
}

int
pgaio_io_get_iovec(PgAioHandle *ioh, struct iovec **iov)
{
	Assert(ioh->state == AHS_HANDED_OUT);

	*iov = &aio_ctl->iovecs[ioh->iovec_off];

	/* AFIXME: Needs to be the value at startup time */
	return io_combine_limit;
}

PgAioSubjectData *
pgaio_io_get_subject_data(PgAioHandle *ioh)
{
	return &ioh->scb_data;
}

PgAioOpData *
pgaio_io_get_op_data(PgAioHandle *ioh)
{
	return &ioh->op_data;
}

ProcNumber
pgaio_io_get_owner(PgAioHandle *ioh)
{
	return ioh->owner_procno;
}

bool
pgaio_io_has_subject(PgAioHandle *ioh)
{
	return ioh->subject != ASI_INVALID;
}

void
pgaio_io_set_flag(PgAioHandle *ioh, PgAioHandleFlags flag)
{
	Assert(ioh->state == AHS_HANDED_OUT);

	ioh->flags |= flag;
}

void
pgaio_io_set_io_data_32(PgAioHandle *ioh, uint32 *data, uint8 len)
{
	Assert(ioh->state == AHS_HANDED_OUT);

	for (int i = 0; i < len; i++)
		aio_ctl->iovecs_data[ioh->iovec_off + i] = data[i];
	ioh->iovec_data_len = len;
}

uint64 *
pgaio_io_get_io_data(PgAioHandle *ioh, uint8 *len)
{
	Assert(ioh->iovec_data_len > 0);

	*len = ioh->iovec_data_len;

	return &aio_ctl->iovecs_data[ioh->iovec_off];
}

void
pgaio_io_set_subject(PgAioHandle *ioh, PgAioSubjectID subjid)
{
	Assert(ioh->state == AHS_HANDED_OUT);

	ioh->subject = subjid;

	elog(DEBUG3, "io:%d, op %s, subject %s, set subject",
		 pgaio_io_get_id(ioh),
		 pgaio_io_get_op_name(ioh),
		 pgaio_io_get_subject_name(ioh));
}

void
pgaio_io_get_ref(PgAioHandle *ioh, PgAioHandleRef *ior)
{
	Assert(ioh->state == AHS_HANDED_OUT ||
		   ioh->state == AHS_PREPARING ||
		   ioh->state == AHS_PREPARED);
	Assert(ioh->generation != 0);

	ior->aio_index = ioh - aio_ctl->io_handles;
	ior->generation_upper = (uint32) (ioh->generation >> 32);
	ior->generation_lower = (uint32) ioh->generation;
}

void
pgaio_io_ref_clear(PgAioHandleRef *ior)
{
	ior->aio_index = PG_UINT32_MAX;
}

bool
pgaio_io_ref_valid(PgAioHandleRef *ior)
{
	return ior->aio_index != PG_UINT32_MAX;
}

int
pgaio_io_ref_get_id(PgAioHandleRef *ior)
{
	Assert(pgaio_io_ref_valid(ior));
	return ior->aio_index;
}

bool
pgaio_io_was_recycled(PgAioHandle *ioh, uint64 ref_generation, PgAioHandleState *state)
{
	*state = ioh->state;
	pg_read_barrier();

	return ioh->generation != ref_generation;
}

void
pgaio_io_ref_wait(PgAioHandleRef *ior)
{
	uint64		ref_generation;
	PgAioHandleState state;
	bool		am_owner;
	PgAioHandle *ioh;

	ioh = pgaio_io_from_ref(ior, &ref_generation);

	am_owner = ioh->owner_procno == MyProcNumber;

	if (pgaio_io_was_recycled(ioh, ref_generation, &state))
		return;

	if (am_owner)
	{
		if (state == AHS_PREPARING || state == AHS_PREPARED)
		{
			/* XXX: Arguably this should be prevented by callers? */
			pgaio_submit_staged();
		}
		else if (state != AHS_IN_FLIGHT
				 && state != AHS_REAPED
				 && state != AHS_COMPLETED_SHARED
				 && state != AHS_COMPLETED_LOCAL)
		{
			elog(PANIC, "waiting for own IO in wrong state: %d",
				 state);
		}

		/*
		 * Somebody else completed the IO, need to execute issuer callback, so
		 * reclaim eagerly.
		 */
		if (state == AHS_COMPLETED_LOCAL)
		{
			pgaio_io_reclaim(ioh);

			return;
		}
	}

	while (true)
	{
		if (pgaio_io_was_recycled(ioh, ref_generation, &state))
			return;

		switch (state)
		{
			case AHS_IDLE:
			case AHS_HANDED_OUT:
				elog(ERROR, "IO in wrong state: %d", state);
				break;

			case AHS_IN_FLIGHT:
				/*
				 * If we need to wait via the IO method, do so now. Don't
				 * check via the IO method if the issuing backend is executing
				 * the IO synchronously.
				 */
				if (pgaio_impl->wait_one && !(ioh->flags & AHF_SYNCHRONOUS))
				{
					pgaio_impl->wait_one(ioh, ref_generation);
					continue;
				}
				/* fallthrough */

				/* waiting for owner to submit */
			case AHS_PREPARING:
			case AHS_PREPARED:
				/* waiting for reaper to complete */
				/* fallthrough */
			case AHS_REAPED:
				/* shouldn't be able to hit this otherwise */
				Assert(IsUnderPostmaster);
				/* ensure we're going to get woken up */
				ConditionVariablePrepareToSleep(&ioh->cv);

				while (!pgaio_io_was_recycled(ioh, ref_generation, &state))
				{
					if (state != AHS_REAPED && state != AHS_IN_FLIGHT)
						break;
					ConditionVariableSleep(&ioh->cv, WAIT_EVENT_AIO_COMPLETION);
				}

				ConditionVariableCancelSleep();
				break;

			case AHS_COMPLETED_SHARED:
				/* see above */
				if (am_owner)
					pgaio_io_reclaim(ioh);
				return;
			case AHS_COMPLETED_LOCAL:
				return;
		}
	}
}

/*
 * Check if the the referenced IO completed, without blocking.
 */
bool
pgaio_io_ref_check_done(PgAioHandleRef *ior)
{
	uint64		ref_generation;
	PgAioHandleState state;
	bool		am_owner;
	PgAioHandle *ioh;

	ioh = pgaio_io_from_ref(ior, &ref_generation);

	if (pgaio_io_was_recycled(ioh, ref_generation, &state))
		return true;


	if (state == AHS_IDLE)
		return true;

	am_owner = ioh->owner_procno == MyProcNumber;

	if (state == AHS_COMPLETED_SHARED || state == AHS_COMPLETED_LOCAL)
	{
		if (am_owner)
			pgaio_io_reclaim(ioh);
		return true;
	}

	return false;
}

int
pgaio_io_get_id(PgAioHandle *ioh)
{
	Assert(ioh >= aio_ctl->io_handles &&
		   ioh <= (aio_ctl->io_handles + aio_ctl->io_handle_count));
	return ioh - aio_ctl->io_handles;
}

const char *
pgaio_io_get_state_name(PgAioHandle *ioh)
{
	switch (ioh->state)
	{
		case AHS_IDLE:
			return "idle";
		case AHS_HANDED_OUT:
			return "handed_out";
		case AHS_PREPARING:
			return "PREPARING";
		case AHS_PREPARED:
			return "PREPARED";
		case AHS_IN_FLIGHT:
			return "IN_FLIGHT";
		case AHS_REAPED:
			return "REAPED";
		case AHS_COMPLETED_SHARED:
			return "COMPLETED_SHARED";
		case AHS_COMPLETED_LOCAL:
			return "COMPLETED_LOCAL";
	}
	pg_unreachable();
}

/*
 * Internal, should only be called from pgaio_io_prep_*().
 *
 * Switches the IO to PREPARING state.
 */
void
pgaio_io_start_staging(PgAioHandle *ioh)
{
	Assert(ioh->state == AHS_HANDED_OUT);
	Assert(pgaio_io_has_subject(ioh));

	ioh->result = 0;

	pgaio_io_update_state(ioh, AHS_PREPARING);
}

/*
 * Handle IO getting completed by a method.
 */
void
pgaio_io_process_completion(PgAioHandle *ioh, int result)
{
	Assert(ioh->state == AHS_IN_FLIGHT);

	ioh->result = result;

	pgaio_io_update_state(ioh, AHS_REAPED);

#ifdef USE_INJECTION_POINTS
	inj_cur_handle = ioh;

	/*
	 * FIXME: This could be in a critical section - but it looks like we can't
	 * just InjectionPointLoad() at process start, as the injection point
	 * might not yet be defined.
	 */
	InjectionPointCached("AIO_PROCESS_COMPLETION_BEFORE_SHARED");

	inj_cur_handle = NULL;
#endif

	pgaio_io_process_completion_subject(ioh);

	pgaio_io_update_state(ioh, AHS_COMPLETED_SHARED);

	/* condition variable broadcast ensures state is visible before wakeup */
	ConditionVariableBroadcast(&ioh->cv);

	if (ioh->owner_procno == MyProcNumber)
		pgaio_io_reclaim(ioh);
}

bool
pgaio_io_needs_synchronous_execution(PgAioHandle *ioh)
{
	if (ioh->flags & AHF_SYNCHRONOUS)
	{
		/* XXX: should we also check if there are other IOs staged? */
		return true;
	}

	if (pgaio_impl->needs_synchronous_execution)
		return pgaio_impl->needs_synchronous_execution(ioh);
	return false;
}

/*
 * Handle IO being processed by IO method.
 */
void
pgaio_io_prepare_submit(PgAioHandle *ioh)
{
	pgaio_io_update_state(ioh, AHS_IN_FLIGHT);

	dclist_push_tail(&my_aio->in_flight_ios, &ioh->node);
}

static inline void
pgaio_io_update_state(PgAioHandle *ioh, PgAioHandleState new_state)
{
	/*
	 * Ensure the changes signified by the new state are visible before the
	 * new state becomes visible.
	 */
	pg_write_barrier();

	ioh->state = new_state;
}

static PgAioHandle *
pgaio_io_from_ref(PgAioHandleRef *ior, uint64 *ref_generation)
{
	PgAioHandle *ioh;

	Assert(ior->aio_index < aio_ctl->io_handle_count);

	ioh = &aio_ctl->io_handles[ior->aio_index];

	*ref_generation = ((uint64) ior->generation_upper) << 32 |
		ior->generation_lower;

	Assert(*ref_generation != 0);

	return ioh;
}

static void
pgaio_io_resowner_register(PgAioHandle *ioh)
{
	Assert(!ioh->resowner);
	Assert(CurrentResourceOwner);

	ResourceOwnerRememberAioHandle(CurrentResourceOwner, &ioh->resowner_node);
	ioh->resowner = CurrentResourceOwner;
}

static void
pgaio_io_reclaim(PgAioHandle *ioh)
{
	/* This is only ok if it's our IO */
	Assert(ioh->owner_procno == MyProcNumber);

	ereport(DEBUG3,
			errmsg("reclaiming io:%d, state: %s, op %s, subject %s, result: %d, distilled_result: AFIXME, report to: %p",
				   pgaio_io_get_id(ioh),
				   pgaio_io_get_state_name(ioh),
				   pgaio_io_get_op_name(ioh),
				   pgaio_io_get_subject_name(ioh),
				   ioh->result,
				   ioh->report_return
				   ),
			errhidestmt(true), errhidecontext(true));

	/* if the IO has been defined, we might need to do more work */
	if (ioh->state != AHS_HANDED_OUT)
	{
		dclist_delete_from(&my_aio->in_flight_ios, &ioh->node);

		if (ioh->report_return)
		{
			ioh->report_return->result = ioh->distilled_result;
			ioh->report_return->subject_data = ioh->scb_data;
		}
	}

	/* reclaim all associated bounce buffers */
	if (!slist_is_empty(&ioh->bounce_buffers))
	{
		slist_mutable_iter it;

		slist_foreach_modify(it, &ioh->bounce_buffers)
		{
			PgAioBounceBuffer *bb = slist_container(PgAioBounceBuffer, node, it.cur);

			slist_delete_current(&it);

			slist_push_head(&my_aio->idle_bbs, &bb->node);
		}
	}

	if (ioh->resowner)
	{
		ResourceOwnerForgetAioHandle(ioh->resowner, &ioh->resowner_node);
		ioh->resowner = NULL;
	}

	Assert(!ioh->resowner);

	ioh->num_shared_callbacks = 0;
	ioh->iovec_data_len = 0;
	ioh->report_return = NULL;
	ioh->flags = 0;

	/* XXX: the barrier is probably superfluous */
	pg_write_barrier();
	ioh->generation++;

	pgaio_io_update_state(ioh, AHS_IDLE);

	/*
	 * We push the IO to the head of the idle IO list, that seems more cache
	 * efficient in cases where only a few IOs are used.
	 */
	dclist_push_head(&my_aio->idle_ios, &ioh->node);
}

static void
pgaio_io_wait_for_free(void)
{
	int			reclaimed = 0;

	elog(DEBUG2,
		 "waiting for self: %d pending",
		 my_aio->num_staged_ios);

	/*
	 * First check if any of our IOs actually have completed - when using
	 * worker, that'll often be the case. We could do so as part of the loop
	 * below, but that'd potentially lead us to wait for some IO submitted
	 * before.
	 */
	for (int i = 0; i < io_max_concurrency; i++)
	{
		PgAioHandle *ioh = &aio_ctl->io_handles[my_aio->io_handle_off + i];

		if (ioh->state == AHS_COMPLETED_SHARED)
		{
			pgaio_io_reclaim(ioh);
			reclaimed++;
		}
	}

	if (reclaimed > 0)
		return;

	/*
	 * If we have any unsubmitted IOs, submit them now. We'll start waiting in
	 * a second, so it's better they're in flight. This also addresses the
	 * edge-case that all IOs are unsubmitted.
	 */
	if (my_aio->num_staged_ios > 0)
	{
		elog(DEBUG2, "submitting while acquiring free io");
		pgaio_submit_staged();
	}

	/*
	 * It's possible that we recognized there were free IOs while submitting.
	 */
	if (dclist_count(&my_aio->in_flight_ios) == 0)
	{
		elog(ERROR, "no free IOs despite no in-flight IOs");
	}

	/*
	 * Wait for the oldest in-flight IO to complete.
	 *
	 * XXX: Reusing the general IO wait is suboptimal, we don't need to wait
	 * for that specific IO to complete, we just need *any* IO to complete.
	 */
	{
		PgAioHandle *ioh = dclist_head_element(PgAioHandle, node, &my_aio->in_flight_ios);

		switch (ioh->state)
		{
			/* should not be in in-flight list */
			case AHS_IDLE:
			case AHS_HANDED_OUT:
			case AHS_PREPARING:
			case AHS_PREPARED:
			case AHS_COMPLETED_LOCAL:
				elog(ERROR, "shouldn't get here with io:%d in state %d",
					 pgaio_io_get_id(ioh), ioh->state);
				break;

			case AHS_REAPED:
			case AHS_IN_FLIGHT:
				{
					PgAioHandleRef ior;

					ior.aio_index = ioh - aio_ctl->io_handles;
					ior.generation_upper = (uint32) (ioh->generation >> 32);
					ior.generation_lower = (uint32) ioh->generation;

					pgaio_io_ref_wait(&ior);
					elog(DEBUG2, "waited for io:%d",
						 pgaio_io_get_id(ioh));
				}
				break;
			case AHS_COMPLETED_SHARED:
				/* it's possible that another backend just finished this IO */
				pgaio_io_reclaim(ioh);
				break;
		}

		if (dclist_count(&my_aio->idle_ios) == 0)
			elog(PANIC, "no idle IOs after waiting");
		return;
	}
}



/* --------------------------------------------------------------------------------
 * Bounce Buffers
 * --------------------------------------------------------------------------------
 */

PgAioBounceBuffer *
pgaio_bounce_buffer_get(void)
{
	PgAioBounceBuffer *bb = NULL;
	slist_node *node;

	if (my_aio->handed_out_bb != NULL)
		elog(ERROR, "can only hand out one BB");

	/*
	 * FIXME It probably is not correct to have bounce buffers be per backend,
	 * they use too much memory.
	 */
	if (slist_is_empty(&my_aio->idle_bbs))
	{
		pgaio_bounce_buffer_wait_for_free();
	}

	node = slist_pop_head_node(&my_aio->idle_bbs);
	bb = slist_container(PgAioBounceBuffer, node, node);

	my_aio->handed_out_bb = bb;

	bb->resowner = CurrentResourceOwner;
	ResourceOwnerRememberAioBounceBuffer(bb->resowner, &bb->resowner_node);

	return bb;
}

void
pgaio_io_assoc_bounce_buffer(PgAioHandle *ioh, PgAioBounceBuffer *bb)
{
	if (my_aio->handed_out_bb != bb)
		elog(ERROR, "can only assign handed out BB");
	my_aio->handed_out_bb = NULL;

	/*
	 * There can be many bounce buffers assigned in case of vectorized IOs.
	 */
	slist_push_head(&ioh->bounce_buffers, &bb->node);

	/* once associated with an IO, the IO has ownership */
	ResourceOwnerForgetAioBounceBuffer(bb->resowner, &bb->resowner_node);
	bb->resowner = NULL;
}

uint32
pgaio_bounce_buffer_id(PgAioBounceBuffer *bb)
{
	return bb - aio_ctl->bounce_buffers;
}

void
pgaio_bounce_buffer_release(PgAioBounceBuffer *bb)
{
	if (my_aio->handed_out_bb != bb)
		elog(ERROR, "can only release handed out BB");

	slist_push_head(&my_aio->idle_bbs, &bb->node);
	my_aio->handed_out_bb = NULL;

	ResourceOwnerForgetAioBounceBuffer(bb->resowner, &bb->resowner_node);
	bb->resowner = NULL;
}

void
pgaio_bounce_buffer_release_resowner(dlist_node *bb_node, bool on_error)
{
	PgAioBounceBuffer *bb = dlist_container(PgAioBounceBuffer, resowner_node, bb_node);

	Assert(bb->resowner);

	if (!on_error)
		elog(WARNING, "leaked AIO bounce buffer");

	pgaio_bounce_buffer_release(bb);
}

char *
pgaio_bounce_buffer_buffer(PgAioBounceBuffer *bb)
{
	return bb->buffer;
}

static void
pgaio_bounce_buffer_wait_for_free(void)
{
	static uint32 lastpos = 0;

	if (my_aio->num_staged_ios > 0)
	{
		elog(DEBUG2, "submitting while acquiring free bb");
		pgaio_submit_staged();
	}

	for (uint32 i = lastpos; i < lastpos + io_max_concurrency; i++)
	{
		uint32		thisoff = my_aio->io_handle_off + (i % io_max_concurrency);
		PgAioHandle *ioh = &aio_ctl->io_handles[thisoff];

		switch (ioh->state)
		{
			case AHS_IDLE:
			case AHS_HANDED_OUT:
				continue;
			case AHS_PREPARING:	/* should have been submitted above */
			case AHS_PREPARED:
				elog(ERROR, "shouldn't get here with io:%d in state %d",
					 pgaio_io_get_id(ioh), ioh->state);
				break;
			case AHS_REAPED:
			case AHS_IN_FLIGHT:
				if (!slist_is_empty(&ioh->bounce_buffers))
				{
					PgAioHandleRef ior;

					ior.aio_index = ioh - aio_ctl->io_handles;
					ior.generation_upper = (uint32) (ioh->generation >> 32);
					ior.generation_lower = (uint32) ioh->generation;

					pgaio_io_ref_wait(&ior);
					elog(DEBUG2, "waited for io:%d to reclaim BB",
						 pgaio_io_get_id(ioh));

					if (slist_is_empty(&my_aio->idle_bbs))
						elog(WARNING, "empty after wait");

					if (!slist_is_empty(&my_aio->idle_bbs))
					{
						lastpos = i;
						return;
					}
				}
				break;
			case AHS_COMPLETED_SHARED:
			case AHS_COMPLETED_LOCAL:
				/* reclaim */
				pgaio_io_reclaim(ioh);

				if (!slist_is_empty(&my_aio->idle_bbs))
				{
					lastpos = i;
					return;
				}
				break;
		}
	}

	/*
	 * The submission above could have caused the IO to complete at any time.
	 */
	if (slist_is_empty(&my_aio->idle_bbs))
		elog(PANIC, "no more bbs");
}



/* --------------------------------------------------------------------------------
 * Actions on multiple IOs.
 * --------------------------------------------------------------------------------
 */

void
pgaio_submit_staged(void)
{
	int			total_submitted = 0;
	int			did_submit;

	if (my_aio->num_staged_ios == 0)
		return;


	START_CRIT_SECTION();

	did_submit = pgaio_impl->submit(my_aio->num_staged_ios, my_aio->staged_ios);

	END_CRIT_SECTION();

	total_submitted += did_submit;

	Assert(total_submitted == did_submit);

	my_aio->num_staged_ios = 0;

#ifdef PGAIO_VERBOSE
	ereport(DEBUG2,
			errmsg("submitted %d",
				   total_submitted),
			errhidestmt(true),
			errhidecontext(true));
#endif
}

bool
pgaio_have_staged(void)
{
	return my_aio->num_staged_ios > 0;
}



/* --------------------------------------------------------------------------------
 * Other
 * --------------------------------------------------------------------------------
 */

/*
 * Need to submit staged but not yet submitted IOs using the fd, otherwise
 * the IO would end up targeting something bogus.
 */
void
pgaio_closing_fd(int fd)
{
	/*
	 * Might be called before AIO is initialized or in a subprocess that
	 * doesn't use AIO.
	 */
	if (!my_aio)
		return;

	/*
	 * For now just submit all staged IOs - we could be more selective, but
	 * it's probably not worth it.
	 */
	pgaio_submit_staged();
}

void
pgaio_at_xact_end(bool is_subxact, bool is_commit)
{
	Assert(!my_aio->handed_out_io);
	Assert(!my_aio->handed_out_bb);
}

/*
 * Similar to pgaio_at_xact_end(..., is_commit = false), but for cases where
 * errors happen outside of transactions.
 */
void
pgaio_at_error(void)
{
	Assert(!my_aio->handed_out_io);
	Assert(!my_aio->handed_out_bb);
}


void
assign_io_method(int newval, void *extra)
{
	pgaio_impl = pgaio_ops_table[newval];
}



/* --------------------------------------------------------------------------------
 * Injection point support
 * --------------------------------------------------------------------------------
 */

#ifdef USE_INJECTION_POINTS

PgAioHandle *
pgaio_inj_io_get(void)
{
	return inj_cur_handle;
}

#endif
