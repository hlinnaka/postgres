/*-------------------------------------------------------------------------
 *
 * aio_subject.c
 *	  AIO - Functionality related to executing IO for different subjects
 *
 * XXX Write me
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio_subject.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/memutils.h"


/*
 * Registry for entities that can be the target of AIO.
 *
 * To support executing using worker processes, the file descriptor for an IO
 * may need to be be reopened in a different process. This is done via the
 * PgAioSubjectInfo.reopen callback.
 */
static const PgAioSubjectInfo *aio_subject_info[] = {
	[ASI_INVALID] = &(PgAioSubjectInfo) {
		.name = "invalid",
	},
};


typedef struct PgAioHandleSharedCallbacksEntry
{
	const PgAioHandleSharedCallbacks *const cb;
	const char *const name;
} PgAioHandleSharedCallbacksEntry;

static const PgAioHandleSharedCallbacksEntry aio_shared_cbs[] = {
#define CALLBACK_ENTRY(id, callback)  [id] = {.cb = &callback, .name = #callback}
#undef CALLBACK_ENTRY
};


/*
 * Register callback for the IO handle.
 *
 * Only a limited number (AIO_MAX_SHARED_CALLBACKS) of callbacks can be
 * registered for each IO.
 *
 * Callbacks need to be registered before [indirectly] calling
 * pgaio_io_prep_*(), as the IO may be executed immediately.
 *
 *
 * Note that callbacks are executed in critical sections.  This is necessary
 * to be able to execute IO in critical sections (consider e.g. WAL
 * logging). To perform AIO we first need to acquire a handle, which, if there
 * are no free handles, requires waiting for IOs to complete and to execute
 * their completion callbacks.
 *
 * Callbacks may be executed in the issuing backend but also in another
 * backend (because that backend is waiting for the IO) or in IO workers (if
 * io_method=worker is used).
 *
 *
 * See PgAioHandleSharedCallbackID's definition for an explanation for why
 * callbacks are not identified by a pointer.
 */
void
pgaio_io_add_shared_cb(PgAioHandle *ioh, PgAioHandleSharedCallbackID cbid)
{
	const PgAioHandleSharedCallbacksEntry *ce = &aio_shared_cbs[cbid];

	if (cbid >= lengthof(aio_shared_cbs))
		elog(ERROR, "callback %d is out of range", cbid);
	if (aio_shared_cbs[cbid].cb->complete == NULL)
		elog(ERROR, "callback %d is undefined", cbid);
	if (ioh->num_shared_callbacks >= AIO_MAX_SHARED_CALLBACKS)
		elog(PANIC, "too many callbacks, the max is %d", AIO_MAX_SHARED_CALLBACKS);
	ioh->shared_callbacks[ioh->num_shared_callbacks] = cbid;

	elog(DEBUG3, "io:%d, op %s, subject %s, adding cb #%d, id %d/%s",
		 pgaio_io_get_id(ioh),
		 pgaio_io_get_op_name(ioh),
		 pgaio_io_get_subject_name(ioh),
		 ioh->num_shared_callbacks + 1,
		 cbid, ce->name);

	ioh->num_shared_callbacks++;
}

/*
 * Return the name for the subject associated with the IO. Mostly useful for
 * debugging/logging.
 */
const char *
pgaio_io_get_subject_name(PgAioHandle *ioh)
{
	Assert(ioh->subject >= 0 && ioh->subject < ASI_COUNT);

	return aio_subject_info[ioh->subject]->name;
}

/*
 * Internal function which invokes ->prepare for all the registered callbacks.
 */
void
pgaio_io_prepare_subject(PgAioHandle *ioh)
{
	Assert(ioh->subject > ASI_INVALID && ioh->subject < ASI_COUNT);
	Assert(ioh->op >= 0 && ioh->op < PGAIO_OP_COUNT);

	for (int i = ioh->num_shared_callbacks; i > 0; i--)
	{
		PgAioHandleSharedCallbackID cbid = ioh->shared_callbacks[i - 1];
		const PgAioHandleSharedCallbacksEntry *ce = &aio_shared_cbs[cbid];

		if (!ce->cb->prepare)
			continue;

		elog(DEBUG3, "io:%d, op %s, subject %s, calling cb #%d %d/%s->prepare",
			 pgaio_io_get_id(ioh),
			 pgaio_io_get_op_name(ioh),
			 pgaio_io_get_subject_name(ioh),
			 i,
			 cbid, ce->name);
		ce->cb->prepare(ioh);
	}
}

/*
 * Internal function which invokes ->complete for all the registered
 * callbacks.
 */
void
pgaio_io_process_completion_subject(PgAioHandle *ioh)
{
	PgAioResult result;

	Assert(ioh->subject >= 0 && ioh->subject < ASI_COUNT);
	Assert(ioh->op >= 0 && ioh->op < PGAIO_OP_COUNT);

	result.status = ARS_OK;		/* low level IO is always considered OK */
	result.result = ioh->result;
	result.id = ASC_INVALID;
	result.error_data = 0;

	for (int i = ioh->num_shared_callbacks; i > 0; i--)
	{
		PgAioHandleSharedCallbackID cbid = ioh->shared_callbacks[i - 1];
		const PgAioHandleSharedCallbacksEntry *ce = &aio_shared_cbs[cbid];

		elog(DEBUG3, "io:%d, op %s, subject %s, calling cb #%d, id %d/%s->complete with distilled result status %d, id %u, error_data: %d, result: %d",
			 pgaio_io_get_id(ioh),
			 pgaio_io_get_op_name(ioh),
			 pgaio_io_get_subject_name(ioh),
			 i,
			 cbid, ce->name,
			 result.status,
			 result.id,
			 result.error_data,
			 result.result);
		result = ce->cb->complete(ioh, result);
	}

	ioh->distilled_result = result;

	elog(DEBUG3, "io:%d, op %s, subject %s, distilled result status %d, id %u, error_data: %d, result: %d, raw_result %d",
		 pgaio_io_get_id(ioh),
		 pgaio_io_get_op_name(ioh),
		 pgaio_io_get_subject_name(ioh),
		 result.status,
		 result.id,
		 result.error_data,
		 result.result,
		 ioh->result);
}

/*
 * Check if pgaio_io_reopen() is available for the IO.
 */
bool
pgaio_io_can_reopen(PgAioHandle *ioh)
{
	return aio_subject_info[ioh->subject]->reopen != NULL;
}

/*
 * Before executing an IO outside of the context of the process the IO has
 * been prepared in, the file descriptor has to be reopened - any FD
 * referenced in the IO itself, won't be valid in the separate process.
 */
void
pgaio_io_reopen(PgAioHandle *ioh)
{
	Assert(ioh->subject >= 0 && ioh->subject < ASI_COUNT);
	Assert(ioh->op >= 0 && ioh->op < PGAIO_OP_COUNT);

	aio_subject_info[ioh->subject]->reopen(ioh);
}



/* --------------------------------------------------------------------------------
 * IO Result
 * --------------------------------------------------------------------------------
 */

void
pgaio_result_log(PgAioResult result, const PgAioSubjectData *subject_data, int elevel)
{
	PgAioHandleSharedCallbackID cbid = result.id;
	const PgAioHandleSharedCallbacksEntry *ce = &aio_shared_cbs[cbid];

	Assert(result.status != ARS_UNKNOWN);
	Assert(result.status != ARS_OK);

	if (ce->cb->error == NULL)
		elog(ERROR, "scb id %d/%s does not have an error callback",
			 result.id, ce->name);

	ce->cb->error(result, subject_data, elevel);
}
