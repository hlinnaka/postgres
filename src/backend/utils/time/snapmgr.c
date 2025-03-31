/*-------------------------------------------------------------------------
 *
 * snapmgr.c
 *		PostgreSQL snapshot manager
 *
 * The following functions return an MVCC snapshot that can be used in tuple
 * visibility checks:
 *
 * - GetTransactionSnapshot
 * - GetLatestSnapshot
 * - GetCatalogSnapshot
 * - GetNonHistoricCatalogSnapshot
 *
 * Each of these functions returns a reference to a statically allocated
 * snapshot.  The statically allocated snapshot is subject to change on any
 * snapshot-related function call, and should not be used directly.  Instead,
 * call PushActiveSnapshot() or RegisterSnapshot() to create a longer-lived
 * copy and use that.
 *
 * We keep track of snapshots in two ways: those "registered" by resowner.c,
 * and the "active snapshot" stack.  All snapshots in either of them live in
 * persistent memory.  When a snapshot is no longer in any of these lists
 * (tracked by separate refcounts on each snapshot), its memory can be freed.
 *
 * In addition to the above-mentioned MVCC snapshots, there are some special
 * snapshots like SnapshotSelf, SnapshotAny, and "dirty" snapshots.  They can
 * only be used in limited contexts and cannot be registered or pushed to the
 * active stack.
 *
 * ActiveSnapshot stack
 * --------------------
 *
 * Most visibility checks use the current "active snapshot" returned by
 * GetActiveSnapshot().  When running normal queries, the active snapshot is
 * set when query execution begins based on the transaction isolation level.
 *
 * The active snapshot is tracked in a stack so that the currently active one
 * is at the top of the stack.  It mirrors the process call stack: whenever we
 * recurse or switch context to fetch rows from a different portal for
 * example, the appropriate snapshot is pushed to become the active snapshot,
 * and popped on return.  Once upon a time, ActiveSnapshot was just a global
 * variable that was saved and restored similar to CurrentMemoryContext, but
 * nowadays it's managed as a separate data structure so that we can keep
 * track of which snapshots are in use and reset MyProc->xmin when there is no
 * active snapshot.
 *
 * However, there are a couple of exceptions where the active snapshot stack
 * does not strictly mirror the call stack:
 *
 * - VACUUM and a few other utility commands manage their own transactions,
 *   which take their own snapshots.  They are called with an active snapshot
 *   set, like most utility commands, but they pop the active snapshot that
 *   was pushed by the caller.  PortalRunUtility knows about the possibility
 *   that the snapshot it pushed is no longer active on return.
 *
 * - When COMMIT or ROLLBACK is executed within a procedure or DO-block, the
 *   active snapshot stack is destroyed, and re-established later when
 *   subsequent statements in the procedure are executed.  There are many
 *   limitations on when in-procedure COMMIT/ROLLBACK is allowed; one such
 *   limitation is that all the snapshots on the active snapshot stack are
 *   known to portals that are being executed, which makes it safe to reset
 *   the stack.  See EnsurePortalSnapshotExists().
 *
 * Registered snapshots
 * --------------------
 *
 * In addition to snapshots pushed to the active snapshot stack, a snapshot
 * can be registered with a resource owner.
 *
 * Xmin tracking
 * -------------
 *
 * All valid snapshots, whether they are "static", included the active stack,
 * or registered with a resource owner, are tracked in a doubly-linked list,
 * ValidSnapshots.  Any snapshots that have been exported by
 * pg_export_snapshot() are also listed there.  (They have regd_count = 1,
 * even though they are not tracked by any resource owner).
 *
 * The list is in xmin order, so that the tail always contains the oldest
 * snapshot.  That let us reset MyProc->xmin when there are no snapshots
 * referenced by this transaction, and advance it when the one with oldest
 * Xmin is no longer referenced.
 *
 * The lifetime of historic snapshots used during logical decoding is managed
 * separately (as they live longer than one xact.c transaction).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/time/snapmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/csn_log.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "datatype/timestamp.h"
#include "miscadmin.h"
#include "port/pg_lfind.h"
#include "storage/fd.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * Define a radix tree implementation to cache CSN lookups in a snapshot.
 *
 * We need only one bit of information for each XID stored in the cache: was
 * the XID running or not.  However, the radix tree implementation uses 8
 * bytes for each entry (on 64-bit machines) even if the value type is smaller
 * than that.  To reduce memory usage, we use uint64 as the value type, and
 * store multiple XIDs in each value.
 *
 * The 64-bit value word holds two bits for each XID: whether the XID is
 * present in the cache or not, and if it's present, whether it's considered
 * as in-progress by the snapshot or not.  So each entry in the radix tree
 * holds the status for 32 XIDs.
 */
#define RT_PREFIX inprogress_cache
#define RT_SCOPE
#define RT_DECLARE
#define RT_DEFINE
#define RT_VALUE_TYPE uint64
#include "lib/radixtree.h"

#define INPROGRESS_CACHE_BITS 2
#define INPROGRESS_CACHE_XIDS_PER_WORD 32

#define INPROGRESS_CACHE_XID_IS_CACHED(word, slotno) \
	((((word) & (UINT64CONST(1) << (slotno)))) != 0)

#define INPROGRESS_CACHE_XID_IS_IN_PROGRESS(word, slotno) \
	((((word) & (UINT64CONST(1) << ((slotno) + 1)))) != 0)

/*
 * CurrentSnapshot points to the only snapshot taken in transaction-snapshot
 * mode, and to the latest one taken in a read-committed transaction.
 * SecondarySnapshot is a snapshot that's always up-to-date as of the current
 * instant, even in transaction-snapshot mode.  It should only be used for
 * special-purpose code (say, RI checking.)  CatalogSnapshot points to an
 * MVCC snapshot intended to be used for catalog scans; we must invalidate it
 * whenever a system catalog change occurs.
 */
static MVCCSnapshotData CurrentSnapshotData = {SNAPSHOT_MVCC};
static MVCCSnapshotData SecondarySnapshotData = {SNAPSHOT_MVCC};
static MVCCSnapshotData CatalogSnapshotData = {SNAPSHOT_MVCC};
SnapshotData SnapshotSelfData = {SNAPSHOT_SELF};
SnapshotData SnapshotAnyData = {SNAPSHOT_ANY};
SnapshotData SnapshotToastData = {SNAPSHOT_TOAST};

/* Pointers to valid snapshots */
static HistoricMVCCSnapshot HistoricSnapshot = NULL;

/*
 * These are updated by GetMVCCSnapshotData.  We initialize them this way
 * for the convenience of TransactionIdIsInProgress: even in bootstrap
 * mode, we don't want it to say that BootstrapTransactionId is in progress.
 */
TransactionId TransactionXmin = FirstNormalTransactionId;
TransactionId RecentXmin = FirstNormalTransactionId;

/* (table, ctid) => (cmin, cmax) mapping during timetravel */
static HTAB *tuplecid_data = NULL;

/*
 * Elements of the active snapshot stack.
 *
 * NB: the code assumes that elements in this list are in non-increasing
 * order of as_level; also, the list must be NULL-terminated.
 */
typedef struct ActiveSnapshotElt
{
	MVCCSnapshotData as_snap;
	int			as_level;
	struct ActiveSnapshotElt *as_next;
} ActiveSnapshotElt;

/* Top of the stack of active snapshots */
static ActiveSnapshotElt *ActiveSnapshot = NULL;

/*
 * Currently valid Snapshots.  Ordered in a heap by xmin, so that we can
 * quickly find the one with lowest xmin, to advance our MyProc->xmin.
 */
static dlist_head ValidSnapshots = DLIST_STATIC_INIT(ValidSnapshots);

/* first GetTransactionSnapshot call in a transaction? */
bool		FirstSnapshotSet = false;

/*
 * Remember the serializable transaction snapshot, if any.  We cannot trust
 * FirstSnapshotSet in combination with IsolationUsesXactSnapshot(), because
 * GUC may be reset before us, changing the value of IsolationUsesXactSnapshot.
 */
static bool FirstXactSnapshotRegistered = false;

/* Define pathname of exported-snapshot files */
#define SNAPSHOT_EXPORT_DIR "pg_snapshots"

/* Structure holding info about exported snapshot. */
typedef struct ExportedSnapshot
{
	char	   *snapfile;
	MVCCSnapshotShared snapshot;
} ExportedSnapshot;

/* Current xact's exported snapshots (a list of ExportedSnapshot structs) */
static List *exportedSnapshots = NIL;

MVCCSnapshotShared latestSnapshotShared = NULL;
MVCCSnapshotShared spareSnapshotShared = NULL;

/* Prototypes for local functions */
static void UpdateStaticMVCCSnapshot(MVCCSnapshot snapshot, MVCCSnapshotShared shared);
static void UnregisterSnapshotNoOwner(Snapshot snapshot);
static void SnapshotResetXmin(void);
static void ReleaseMVCCSnapshotShared(MVCCSnapshotShared shared);
static void valid_snapshots_push_tail(MVCCSnapshotShared snapshot);
static void valid_snapshots_push_out_of_order(MVCCSnapshotShared snapshot);


/* ResourceOwner callbacks to track snapshot references */
static void ResOwnerReleaseSnapshot(Datum res);

static const ResourceOwnerDesc snapshot_resowner_desc =
{
	.name = "snapshot reference",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_SNAPSHOT_REFS,
	.ReleaseResource = ResOwnerReleaseSnapshot,
	.DebugPrint = NULL			/* the default message is fine */
};

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void
ResourceOwnerRememberSnapshot(ResourceOwner owner, Snapshot snap)
{
	ResourceOwnerRemember(owner, PointerGetDatum(snap), &snapshot_resowner_desc);
}
static inline void
ResourceOwnerForgetSnapshot(ResourceOwner owner, Snapshot snap)
{
	ResourceOwnerForget(owner, PointerGetDatum(snap), &snapshot_resowner_desc);
}

/*
 * Snapshot fields to be serialized.
 *
 * Only these fields need to be sent to the cooperating backend; the
 * remaining ones can (and must) be set by the receiver upon restore.
 */
typedef struct SerializedSnapshotData
{
	TransactionId xmin;
	TransactionId xmax;
	uint32		xcnt;
	int32		subxcnt;
	bool		suboverflowed;
	bool		takenDuringRecovery;
	XLogRecPtr	snapshotCsn;
	CommandId	curcid;
} SerializedSnapshotData;

/*
 * GetTransactionSnapshot
 *		Get the appropriate snapshot for a new query in a transaction.
 *
 * Note that the return value points at static storage that will be modified
 * by future calls and by CommandCounterIncrement().  Callers must call
 * RegisterSnapshot or PushActiveSnapshot on the returned snap before doing
 * any other non-trivial work that could invalidate it.
 */
Snapshot
GetTransactionSnapshot(void)
{
	/*
	 * This should not be called while doing logical decoding.  Historic
	 * snapshots are only usable for catalog access, not for general-purpose
	 * queries.
	 */
	if (HistoricSnapshotActive())
		elog(ERROR, "cannot take query snapshot during logical decoding");

	/* First call in transaction? */
	if (!FirstSnapshotSet)
	{
		MVCCSnapshotShared shared;

		/*
		 * Don't allow catalog snapshot to be older than xact snapshot.  Must
		 * do this first to allow the empty-heap Assert to succeed.
		 */
		InvalidateCatalogSnapshot();

		Assert(dlist_is_empty(&ValidSnapshots));
		Assert(!FirstXactSnapshotRegistered);

		if (IsInParallelMode())
			elog(ERROR,
				 "cannot take query snapshot during a parallel operation");

		/*
		 * In transaction-snapshot mode, the first snapshot must live until
		 * end of xact regardless of what the caller does with it, so we keep
		 * it in RegisteredSnapshots even though it's not tracked by any
		 * resource owner.  Furthermore, if we're running in serializable
		 * mode, predicate.c needs to wrap the snapshot fetch in its own
		 * processing.
		 */
		if (IsolationIsSerializable())
			shared = GetSerializableTransactionSnapshotData();
		else
			shared = GetMVCCSnapshotData();

		UpdateStaticMVCCSnapshot(&CurrentSnapshotData, shared);

		if (IsolationUsesXactSnapshot())
		{
			/* keep it */
			FirstXactSnapshotRegistered = true;
		}
		FirstSnapshotSet = true;
		return (Snapshot) &CurrentSnapshotData;
	}

	if (IsolationUsesXactSnapshot())
	{
		Assert(FirstXactSnapshotRegistered);
		Assert(CurrentSnapshotData.valid);
		return (Snapshot) &CurrentSnapshotData;
	}

	/* Don't allow catalog snapshot to be older than xact snapshot. */
	InvalidateCatalogSnapshot();

	UpdateStaticMVCCSnapshot(&CurrentSnapshotData, GetMVCCSnapshotData());
	return (Snapshot) &CurrentSnapshotData;
}

/*
 * Update a static snapshot with the given shared struct.
 *
 * If the static snapshot is previously valid, release its old 'shared'
 * struct first.
 */
static void
UpdateStaticMVCCSnapshot(MVCCSnapshot snapshot, MVCCSnapshotShared shared)
{
	/* Replace the 'shared' struct */
	if (snapshot->shared)
		ReleaseMVCCSnapshotShared(snapshot->shared);
	snapshot->shared = shared;
	snapshot->shared->refcount++;
	if (snapshot->shared->refcount == 1)
		valid_snapshots_push_tail(shared);

	snapshot->curcid = GetCurrentCommandId(false);
	snapshot->valid = true;
}

/*
 * GetLatestSnapshot
 *		Get a snapshot that is up-to-date as of the current instant,
 *		even if we are executing in transaction-snapshot mode.
 */
Snapshot
GetLatestSnapshot(void)
{
	/*
	 * We might be able to relax this, but nothing that could otherwise work
	 * needs it.
	 */
	if (IsInParallelMode())
		elog(ERROR,
			 "cannot update SecondarySnapshot during a parallel operation");

	/*
	 * So far there are no cases requiring support for GetLatestSnapshot()
	 * during logical decoding, but it wouldn't be hard to add if required.
	 */
	Assert(!HistoricSnapshotActive());

	/* If first call in transaction, go ahead and set the xact snapshot */
	if (!FirstSnapshotSet)
		return GetTransactionSnapshot();

	UpdateStaticMVCCSnapshot(&SecondarySnapshotData, GetMVCCSnapshotData());

	return (Snapshot) &SecondarySnapshotData;
}

/*
 * GetCatalogSnapshot
 *		Get a snapshot that is sufficiently up-to-date for scan of the
 *		system catalog with the specified OID.
 */
Snapshot
GetCatalogSnapshot(Oid relid)
{
	/*
	 * Return historic snapshot while we're doing logical decoding, so we can
	 * see the appropriate state of the catalog.
	 *
	 * This is the primary reason for needing to reset the system caches after
	 * finishing decoding.
	 */
	if (HistoricSnapshotActive())
		return (Snapshot) HistoricSnapshot;

	return GetNonHistoricCatalogSnapshot(relid);
}

/*
 * GetNonHistoricCatalogSnapshot
 *		Get a snapshot that is sufficiently up-to-date for scan of the system
 *		catalog with the specified OID, even while historic snapshots are set
 *		up.
 */
Snapshot
GetNonHistoricCatalogSnapshot(Oid relid)
{
	/*
	 * If the caller is trying to scan a relation that has no syscache, no
	 * catcache invalidations will be sent when it is updated.  For a few key
	 * relations, snapshot invalidations are sent instead.  If we're trying to
	 * scan a relation for which neither catcache nor snapshot invalidations
	 * are sent, we must refresh the snapshot every time.
	 */
	if (CatalogSnapshotData.valid &&
		!RelationInvalidatesSnapshotsOnly(relid) &&
		!RelationHasSysCache(relid))
		InvalidateCatalogSnapshot();

	if (!CatalogSnapshotData.valid)
	{
		/* Get new snapshot. */
		UpdateStaticMVCCSnapshot(&CatalogSnapshotData, GetMVCCSnapshotData());

		/*
		 * Make sure the catalog snapshot will be accounted for in decisions
		 * about advancing PGPROC->xmin.  We could apply RegisterSnapshot, but
		 * that would result in making a physical copy, which is overkill; and
		 * it would also create a dependency on some resource owner, which we
		 * do not want for reasons explained at the head of this file. Instead
		 * just shove the CatalogSnapshot into the pairing heap manually. This
		 * has to be reversed in InvalidateCatalogSnapshot, of course.
		 *
		 * NB: it had better be impossible for this to throw error, since the
		 * CatalogSnapshot pointer is already valid.
		 */
	}

	return (Snapshot) &CatalogSnapshotData;
}

/*
 * InvalidateCatalogSnapshot
 *		Mark the current catalog snapshot, if any, as invalid
 *
 * We could change this API to allow the caller to provide more fine-grained
 * invalidation details, so that a change to relation A wouldn't prevent us
 * from using our cached snapshot to scan relation B, but so far there's no
 * evidence that the CPU cycles we spent tracking such fine details would be
 * well-spent.
 */
void
InvalidateCatalogSnapshot(void)
{
	if (CatalogSnapshotData.valid)
	{
		ReleaseMVCCSnapshotShared(CatalogSnapshotData.shared);
		CatalogSnapshotData.shared = NULL;
		CatalogSnapshotData.valid = false;
	}
	if (!FirstXactSnapshotRegistered && CurrentSnapshotData.valid)
	{
		ReleaseMVCCSnapshotShared(CurrentSnapshotData.shared);
		CurrentSnapshotData.shared = NULL;
		CurrentSnapshotData.valid = false;
	}
	if (SecondarySnapshotData.valid)
	{
		ReleaseMVCCSnapshotShared(SecondarySnapshotData.shared);
		SecondarySnapshotData.shared = NULL;
		SecondarySnapshotData.valid = false;
	}

	SnapshotResetXmin();
}

/*
 * InvalidateCatalogSnapshotConditionally
 *		Drop catalog snapshot if it's the only one we have
 *
 * This is called when we are about to wait for client input, so we don't
 * want to continue holding the catalog snapshot if it might mean that the
 * global xmin horizon can't advance.  However, if there are other snapshots
 * still active or registered, the catalog snapshot isn't likely to be the
 * oldest one, so we might as well keep it. XXX
 */
void
InvalidateCatalogSnapshotConditionally(void)
{
	if (CatalogSnapshotData.valid &&
		dlist_tail_node(&ValidSnapshots) == &CatalogSnapshotData.shared->node &&
		CatalogSnapshotData.shared->refcount == 1)
		InvalidateCatalogSnapshot();
}

/*
 * SnapshotSetCommandId
 *		Propagate CommandCounterIncrement into the static snapshots, if set
 */
void
SnapshotSetCommandId(CommandId curcid)
{
	if (!FirstSnapshotSet)
		return;

	if (CurrentSnapshotData.valid)
		CurrentSnapshotData.curcid = curcid;
	if (SecondarySnapshotData.valid)
		SecondarySnapshotData.curcid = curcid;
	/* Should we do the same with CatalogSnapshot? */
}

/*
 * SetTransactionSnapshot
 *		Set the transaction's snapshot from an imported MVCC snapshot.
 *
 * Note that this is very closely tied to GetTransactionSnapshot --- it
 * must take care of all the same considerations as the first-snapshot case
 * in GetTransactionSnapshot.
 */
static void
SetTransactionSnapshot(MVCCSnapshotShared sourcesnap, VirtualTransactionId *sourcevxid,
					   int sourcepid, PGPROC *sourceproc)
{
	/* Caller should have checked this already */
	Assert(!FirstSnapshotSet);

	/* Better do this to ensure following Assert succeeds. */
	InvalidateCatalogSnapshot();

	Assert(!FirstXactSnapshotRegistered);
	Assert(!HistoricSnapshotActive());
	Assert(sourcesnap->refcount > 0);

	/*
	 * Even though we are not going to use the snapshot it computes, we must
	 * call GetMVCCSnapshotData to update the state for GlobalVis*.
	 */
	UpdateStaticMVCCSnapshot(&CurrentSnapshotData, GetMVCCSnapshotData());

	/*
	 * Now copy appropriate fields from the source snapshot.
	 */
	ReleaseMVCCSnapshotShared(CurrentSnapshotData.shared);
	CurrentSnapshotData.shared = sourcesnap;
	CurrentSnapshotData.shared->refcount++;

	/* NB: curcid should NOT be copied, it's a local matter */

	/*
	 * Now we have to fix what GetMVCCSnapshotData did with MyProc->xmin and
	 * TransactionXmin.  There is a race condition: to make sure we are not
	 * causing the global xmin to go backwards, we have to test that the
	 * source transaction is still running, and that has to be done
	 * atomically. So let procarray.c do it.
	 *
	 * Note: in serializable mode, predicate.c will do this a second time. It
	 * doesn't seem worth contorting the logic here to avoid two calls,
	 * especially since it's not clear that predicate.c *must* do this.
	 */
	if (sourceproc != NULL)
	{
		if (!ProcArrayInstallRestoredXmin(CurrentSnapshotData.shared->xmin, sourceproc))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("could not import the requested snapshot"),
					 errdetail("The source transaction is not running anymore.")));
	}
	else if (!ProcArrayInstallImportedXmin(CurrentSnapshotData.shared->xmin, sourcevxid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not import the requested snapshot"),
				 errdetail("The source process with PID %d is not running anymore.",
						   sourcepid)));

	/*
	 * In transaction-snapshot mode, the first snapshot must live until end of
	 * xact, so we include it in RegisteredSnapshots.  Furthermore, if we're
	 * running in serializable mode, predicate.c needs to do its own
	 * processing.
	 */
	if (IsolationUsesXactSnapshot())
	{
		if (IsolationIsSerializable())
			SetSerializableTransactionSnapshotData(CurrentSnapshotData.shared,
												   sourcevxid, sourcepid);
		/* keep it */
		FirstXactSnapshotRegistered = true;
	}

	FirstSnapshotSet = true;
}

/*
 * PushActiveSnapshot
 *		Set the given snapshot as the current active snapshot
 *
 * If the passed snapshot is a statically-allocated one, or it is possibly
 * subject to a future command counter update, create a new long-lived copy
 * with active refcount=1.  Otherwise, only increment the refcount. XXX
 *
 * Only regular MVCC snaphots can be used as the active snapshot.
 */
void
PushActiveSnapshot(Snapshot snapshot)
{
	PushActiveSnapshotWithLevel(snapshot, GetCurrentTransactionNestLevel());
}

/*
 * PushActiveSnapshotWithLevel
 *		Set the given snapshot as the current active snapshot
 *
 * Same as PushActiveSnapshot except that caller can specify the
 * transaction nesting level that "owns" the snapshot.  This level
 * must not be deeper than the current top of the snapshot stack.
 */
void
PushActiveSnapshotWithLevel(Snapshot snapshot, int snap_level)
{
	MVCCSnapshot origsnap;
	ActiveSnapshotElt *newactive;

	Assert(snapshot->snapshot_type == SNAPSHOT_MVCC);
	origsnap = &snapshot->mvcc;
	Assert(origsnap->valid);

	Assert(ActiveSnapshot == NULL || snap_level >= ActiveSnapshot->as_level);

	newactive = MemoryContextAlloc(TopTransactionContext, sizeof(ActiveSnapshotElt));
	memcpy(&newactive->as_snap, origsnap, sizeof(MVCCSnapshotData));
	newactive->as_snap.kind = SNAPSHOT_ACTIVE;
	newactive->as_snap.shared->refcount++;

	newactive->as_next = ActiveSnapshot;
	newactive->as_level = snap_level;

	ActiveSnapshot = newactive;
}

/*
 * PushCopiedSnapshot
 *		As above, except forcibly copy the presented snapshot.
 *
 * This should be used when the ActiveSnapshot has to be modifiable, for
 * example if the caller intends to call UpdateActiveSnapshotCommandId.
 * The new snapshot will be released when popped from the stack.
 */
void
PushCopiedSnapshot(Snapshot snapshot)
{
	Assert(snapshot->snapshot_type == SNAPSHOT_MVCC);

	/*
	 * This used to be different from PushActiveSnapshot, but these days
	 * PushActiveSnapshot creates a copy too and there's no difference.
	 */
	PushActiveSnapshot(snapshot);
}

/*
 * UpdateActiveSnapshotCommandId
 *
 * Update the current CID of the active snapshot.  This can only be applied
 * to a snapshot that is not referenced elsewhere. XXX
 */
void
UpdateActiveSnapshotCommandId(void)
{
	CommandId	save_curcid,
				curcid;

	Assert(ActiveSnapshot != NULL);

	/*
	 * Don't allow modification of the active snapshot during parallel
	 * operation.  We share the snapshot to worker backends at the beginning
	 * of parallel operation, so any change to the snapshot can lead to
	 * inconsistencies.  We have other defenses against
	 * CommandCounterIncrement, but there are a few places that call this
	 * directly, so we put an additional guard here.
	 */
	save_curcid = ActiveSnapshot->as_snap.curcid;
	curcid = GetCurrentCommandId(false);
	if (IsInParallelMode() && save_curcid != curcid)
		elog(ERROR, "cannot modify commandid in active snapshot during a parallel operation");

	ActiveSnapshot->as_snap.curcid = curcid;
}

/*
 * PopActiveSnapshot
 *
 * Remove the topmost snapshot from the active snapshot stack, decrementing the
 * reference count, and free it if this was the last reference.
 */
void
PopActiveSnapshot(void)
{
	ActiveSnapshotElt *newstack;

	newstack = ActiveSnapshot->as_next;

	ReleaseMVCCSnapshotShared(ActiveSnapshot->as_snap.shared);

	pfree(ActiveSnapshot);
	ActiveSnapshot = newstack;

	SnapshotResetXmin();
}

/*
 * GetActiveSnapshot
 *		Return the topmost snapshot in the Active stack.
 */
Snapshot
GetActiveSnapshot(void)
{
	Assert(ActiveSnapshot != NULL);

	return (Snapshot) &ActiveSnapshot->as_snap;
}

/*
 * ActiveSnapshotSet
 *		Return whether there is at least one snapshot in the Active stack
 */
bool
ActiveSnapshotSet(void)
{
	return ActiveSnapshot != NULL;
}

/*
 * RegisterSnapshot
 *		Register a snapshot as being in use by the current resource owner
 *
 * Only regular MVCC snaphots and "historic" MVCC snapshots can be registered.
 * InvalidSnapshot is also accepted, as a no-op.
 */
Snapshot
RegisterSnapshot(Snapshot snapshot)
{
	if (snapshot == InvalidSnapshot)
		return InvalidSnapshot;

	return RegisterSnapshotOnOwner(snapshot, CurrentResourceOwner);
}

/*
 * RegisterSnapshotOnOwner
 *		As above, but use the specified resource owner
 */
Snapshot
RegisterSnapshotOnOwner(Snapshot orig_snapshot, ResourceOwner owner)
{
	MVCCSnapshot newsnap;

	if (orig_snapshot == InvalidSnapshot)
		return InvalidSnapshot;

	if (orig_snapshot->snapshot_type == SNAPSHOT_HISTORIC_MVCC)
	{
		HistoricMVCCSnapshot historicsnap = &orig_snapshot->historic_mvcc;

		ResourceOwnerEnlarge(owner);
		historicsnap->regd_count++;
		ResourceOwnerRememberSnapshot(owner, (Snapshot) historicsnap);

		return (Snapshot) historicsnap;
	}

	Assert(orig_snapshot->snapshot_type == SNAPSHOT_MVCC);
	Assert(orig_snapshot->mvcc.valid);

	/* Create a copy */
	newsnap = MemoryContextAlloc(TopTransactionContext, sizeof(MVCCSnapshotData));
	memcpy(newsnap, &orig_snapshot->mvcc, sizeof(MVCCSnapshotData));
	newsnap->kind = SNAPSHOT_REGISTERED;
	newsnap->shared->refcount++;

	/* and tell resowner.c about it */
	ResourceOwnerEnlarge(owner);
	ResourceOwnerRememberSnapshot(owner, (Snapshot) newsnap);

	return (Snapshot) newsnap;
}

/*
 * UnregisterSnapshot
 *
 * Decrement the reference count of a snapshot, remove the corresponding
 * reference from CurrentResourceOwner, and free the snapshot if no more
 * references remain.
 */
void
UnregisterSnapshot(Snapshot snapshot)
{
	if (snapshot == NULL)
		return;

	UnregisterSnapshotFromOwner(snapshot, CurrentResourceOwner);
}

/*
 * UnregisterSnapshotFromOwner
 *		As above, but use the specified resource owner
 */
void
UnregisterSnapshotFromOwner(Snapshot snapshot, ResourceOwner owner)
{
	if (snapshot == NULL)
		return;

	ResourceOwnerForgetSnapshot(owner, snapshot);
	UnregisterSnapshotNoOwner(snapshot);
}

static void
UnregisterSnapshotNoOwner(Snapshot snapshot)
{
	if (snapshot->snapshot_type == SNAPSHOT_MVCC)
	{
		Assert(snapshot->mvcc.kind == SNAPSHOT_REGISTERED);
		Assert(!dlist_is_empty(&ValidSnapshots));

		ReleaseMVCCSnapshotShared(snapshot->mvcc.shared);
		pfree(snapshot);
		SnapshotResetXmin();
	}
	else if (snapshot->snapshot_type == SNAPSHOT_HISTORIC_MVCC)
	{
		HistoricMVCCSnapshot historicsnap = &snapshot->historic_mvcc;

		/*
		 * Historic snapshots don't rely on the resource owner machinery for
		 * cleanup, the snapbuild.c machinery ensures that whenever a historic
		 * snapshot is in use, it has a non-zero refcount.  Registration is
		 * only supported so that the callers don't need to treat regular MVCC
		 * catalog snapshots and historic snapshots differently.
		 */
		Assert(historicsnap->refcount > 0);

		Assert(historicsnap->regd_count > 0);
		historicsnap->regd_count--;
	}
	else
		elog(ERROR, "registered snapshot has unexpected type");
}

/*
 * SnapshotResetXmin
 *
 * If there are no more snapshots, we can reset our PGPROC->xmin to
 * InvalidTransactionId. Note we can do this without locking because we assume
 * that storing an Xid is atomic.
 *
 * Even if there are some remaining snapshots, we may be able to advance our
 * PGPROC->xmin to some degree.  This typically happens when a portal is
 * dropped.  For efficiency, we only consider recomputing PGPROC->xmin when
 * the active snapshot stack is empty; this allows us not to need to track
 * which active snapshot is oldest.
 */
static void
SnapshotResetXmin(void)
{
	MVCCSnapshotShared minSnapshot;

	/*
	 * Invalidate these static snapshots so that we can advance xmin.
	 */
	if (!FirstXactSnapshotRegistered && CurrentSnapshotData.valid)
	{
		ReleaseMVCCSnapshotShared(CurrentSnapshotData.shared);
		CurrentSnapshotData.shared = NULL;
		CurrentSnapshotData.valid = false;
	}
	if (SecondarySnapshotData.valid)
	{
		ReleaseMVCCSnapshotShared(SecondarySnapshotData.shared);
		SecondarySnapshotData.shared = NULL;
		SecondarySnapshotData.valid = false;
	}

	if (ActiveSnapshot != NULL)
		return;

	if (dlist_is_empty(&ValidSnapshots))
	{
		MyProc->xmin = TransactionXmin = InvalidTransactionId;
		return;
	}

	minSnapshot = dlist_head_element(MVCCSnapshotSharedData, node, &ValidSnapshots);

	if (TransactionIdPrecedes(MyProc->xmin, minSnapshot->xmin))
		MyProc->xmin = TransactionXmin = minSnapshot->xmin;
}

/*
 * AtSubCommit_Snapshot
 */
void
AtSubCommit_Snapshot(int level)
{
	ActiveSnapshotElt *active;

	/*
	 * Relabel the active snapshots set in this subtransaction as though they
	 * are owned by the parent subxact.
	 */
	for (active = ActiveSnapshot; active != NULL; active = active->as_next)
	{
		if (active->as_level < level)
			break;
		active->as_level = level - 1;
	}
}

/*
 * AtSubAbort_Snapshot
 *		Clean up snapshots after a subtransaction abort
 */
void
AtSubAbort_Snapshot(int level)
{
	/* Forget the active snapshots set by this subtransaction */
	while (ActiveSnapshot && ActiveSnapshot->as_level >= level)
	{
		ActiveSnapshotElt *next;

		next = ActiveSnapshot->as_next;

		ReleaseMVCCSnapshotShared(ActiveSnapshot->as_snap.shared);
		pfree(ActiveSnapshot);

		ActiveSnapshot = next;
	}

	SnapshotResetXmin();
}

/*
 * AtEOXact_Snapshot
 *		Snapshot manager's cleanup function for end of transaction
 */
void
AtEOXact_Snapshot(bool isCommit, bool resetXmin)
{
	dlist_mutable_iter iter;

	/*
	 * If we exported any snapshots, clean them up.
	 */
	if (exportedSnapshots != NIL)
	{
		ListCell   *lc;

		/*
		 * Get rid of the files.  Unlink failure is only a WARNING because (1)
		 * it's too late to abort the transaction, and (2) leaving a leaked
		 * file around has little real consequence anyway.
		 *
		 * We also need to remove the snapshots from ValidSnapshots to prevent
		 * a warning below.
		 *
		 * As with the FirstXactSnapshot, we don't need to free resources of
		 * the snapshot itself as it will go away with the memory context.
		 */
		foreach(lc, exportedSnapshots)
		{
			ExportedSnapshot *esnap = (ExportedSnapshot *) lfirst(lc);

			if (unlink(esnap->snapfile))
				elog(WARNING, "could not unlink file \"%s\": %m",
					 esnap->snapfile);

			ReleaseMVCCSnapshotShared(esnap->snapshot);
		}

		exportedSnapshots = NIL;
	}

	/* Drop all static snapshot */
	if (CatalogSnapshotData.valid)
	{
		ReleaseMVCCSnapshotShared(CatalogSnapshotData.shared);
		CatalogSnapshotData.shared = NULL;
		CatalogSnapshotData.valid = false;
	}
	if (CurrentSnapshotData.valid)
	{
		ReleaseMVCCSnapshotShared(CurrentSnapshotData.shared);
		CurrentSnapshotData.shared = NULL;
		CurrentSnapshotData.valid = false;
	}
	if (SecondarySnapshotData.valid)
	{
		ReleaseMVCCSnapshotShared(SecondarySnapshotData.shared);
		SecondarySnapshotData.shared = NULL;
		SecondarySnapshotData.valid = false;
	}

	/* On commit, complain about leftover snapshots */
	if (isCommit)
	{
		ActiveSnapshotElt *active;

		if (!dlist_is_empty(&ValidSnapshots))
			elog(WARNING, "registered snapshots seem to remain after cleanup");

		/* complain about unpopped active snapshots */
		for (active = ActiveSnapshot; active != NULL; active = active->as_next)
			elog(WARNING, "snapshot %p still active", active);
	}

	/*
	 * And reset our state.  We don't need to free the memory explicitly --
	 * it'll go away with TopTransactionContext.
	 */
	dlist_foreach_modify(iter, &ValidSnapshots)
	{
		MVCCSnapshotShared cur = dlist_container(MVCCSnapshotSharedData, node, iter.cur);

		dlist_delete(iter.cur);
		cur->refcount = 0;
		if (cur == latestSnapshotShared)
		{
			/* keep it */
		}
		else if (spareSnapshotShared == NULL)
			spareSnapshotShared = cur;
		else
			pfree(cur);
	}

	ActiveSnapshot = NULL;
	FirstSnapshotSet = false;
	FirstXactSnapshotRegistered = false;

	/*
	 * During normal commit processing, we call ProcArrayEndTransaction() to
	 * reset the MyProc->xmin. That call happens prior to the call to
	 * AtEOXact_Snapshot(), so we need not touch xmin here at all.
	 */
	if (resetXmin)
		SnapshotResetXmin();

	Assert(resetXmin || MyProc->xmin == 0);
}


/*
 * ExportSnapshot
 *		Export the snapshot to a file so that other backends can import it.
 *		Returns the token (the file name) that can be used to import this
 *		snapshot.
 */
char *
ExportSnapshot(MVCCSnapshotShared snapshot)
{
	TransactionId topXid;
	TransactionId *children;
	ExportedSnapshot *esnap;
	int			nchildren;
	int			addTopXid;
	StringInfoData buf;
	FILE	   *f;
	int			i;
	MemoryContext oldcxt;
	char		path[MAXPGPATH];
	char		pathtmp[MAXPGPATH];

	/*
	 * It's tempting to call RequireTransactionBlock here, since it's not very
	 * useful to export a snapshot that will disappear immediately afterwards.
	 * However, we haven't got enough information to do that, since we don't
	 * know if we're at top level or not.  For example, we could be inside a
	 * plpgsql function that is going to fire off other transactions via
	 * dblink.  Rather than disallow perfectly legitimate usages, don't make a
	 * check.
	 *
	 * Also note that we don't make any restriction on the transaction's
	 * isolation level; however, importers must check the level if they are
	 * serializable.
	 */

	/*
	 * Get our transaction ID if there is one, to include in the snapshot.
	 */
	topXid = GetTopTransactionIdIfAny();

	/*
	 * We cannot export a snapshot from a subtransaction because there's no
	 * easy way for importers to verify that the same subtransaction is still
	 * running.
	 */
	if (IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot export a snapshot from a subtransaction")));

	/*
	 * We do however allow previous committed subtransactions to exist.
	 * Importers of the snapshot must see them as still running, so get their
	 * XIDs to add them to the snapshot.
	 */
	nchildren = xactGetCommittedChildren(&children);

	/*
	 * Generate file path for the snapshot.  We start numbering of snapshots
	 * inside the transaction from 1.
	 */
	snprintf(path, sizeof(path), SNAPSHOT_EXPORT_DIR "/%08X-%08X-%d",
			 MyProc->vxid.procNumber, MyProc->vxid.lxid,
			 list_length(exportedSnapshots) + 1);

	/*
	 * Copy the snapshot into TopTransactionContext, add it to the
	 * exportedSnapshots list, and mark it pseudo-registered.  We do this to
	 * ensure that the snapshot's xmin is honored for the rest of the
	 * transaction. XXX
	 */
	oldcxt = MemoryContextSwitchTo(TopTransactionContext);
	esnap = (ExportedSnapshot *) palloc(sizeof(ExportedSnapshot));
	esnap->snapfile = pstrdup(path);
	esnap->snapshot = snapshot;
	snapshot->refcount++;
	exportedSnapshots = lappend(exportedSnapshots, esnap);
	MemoryContextSwitchTo(oldcxt);

	/*
	 * Fill buf with a text serialization of the snapshot, plus identification
	 * data about this transaction.  The format expected by ImportSnapshot is
	 * pretty rigid: each line must be fieldname:value.
	 */
	initStringInfo(&buf);

	appendStringInfo(&buf, "vxid:%d/%u\n", MyProc->vxid.procNumber, MyProc->vxid.lxid);
	appendStringInfo(&buf, "pid:%d\n", MyProcPid);
	appendStringInfo(&buf, "dbid:%u\n", MyDatabaseId);
	appendStringInfo(&buf, "iso:%d\n", XactIsoLevel);
	appendStringInfo(&buf, "ro:%d\n", XactReadOnly);

	appendStringInfo(&buf, "xmin:%u\n", snapshot->xmin);
	appendStringInfo(&buf, "xmax:%u\n", snapshot->xmax);

	/*
	 * We must include our own top transaction ID in the top-xid data, since
	 * by definition we will still be running when the importing transaction
	 * adopts the snapshot, but GetMVCCSnapshotData never includes our own XID
	 * in the snapshot.  (There must, therefore, be enough room to add it.)
	 *
	 * However, it could be that our topXid is after the xmax, in which case
	 * we shouldn't include it because xip[] members are expected to be before
	 * xmax.  (We need not make the same check for subxip[] members, see
	 * snapshot.h.)
	 */
	addTopXid = (TransactionIdIsValid(topXid) &&
				 TransactionIdPrecedes(topXid, snapshot->xmax)) ? 1 : 0;
	appendStringInfo(&buf, "xcnt:%d\n", snapshot->xcnt + addTopXid);
	for (i = 0; i < snapshot->xcnt; i++)
		appendStringInfo(&buf, "xip:%u\n", snapshot->xip[i]);
	if (addTopXid)
		appendStringInfo(&buf, "xip:%u\n", topXid);

	/*
	 * Similarly, we add our subcommitted child XIDs to the subxid data. Here,
	 * we have to cope with possible overflow.
	 */
	if (snapshot->suboverflowed ||
		snapshot->subxcnt + nchildren > GetMaxSnapshotSubxidCount())
		appendStringInfoString(&buf, "sof:1\n");
	else
	{
		appendStringInfoString(&buf, "sof:0\n");
		appendStringInfo(&buf, "sxcnt:%d\n", snapshot->subxcnt + nchildren);
		for (i = 0; i < snapshot->subxcnt; i++)
			appendStringInfo(&buf, "sxp:%u\n", snapshot->subxip[i]);
		for (i = 0; i < nchildren; i++)
			appendStringInfo(&buf, "sxp:%u\n", children[i]);
	}
	appendStringInfo(&buf, "rec:%u\n", snapshot->takenDuringRecovery);
	appendStringInfo(&buf, "snapshotcsn:%X/%X\n", LSN_FORMAT_ARGS(snapshot->snapshotCsn));

	/*
	 * Now write the text representation into a file.  We first write to a
	 * ".tmp" filename, and rename to final filename if no error.  This
	 * ensures that no other backend can read an incomplete file
	 * (ImportSnapshot won't allow it because of its valid-characters check).
	 */
	snprintf(pathtmp, sizeof(pathtmp), "%s.tmp", path);
	if (!(f = AllocateFile(pathtmp, PG_BINARY_W)))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", pathtmp)));

	if (fwrite(buf.data, buf.len, 1, f) != 1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", pathtmp)));

	/* no fsync() since file need not survive a system crash */

	if (FreeFile(f))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", pathtmp)));

	/*
	 * Now that we have written everything into a .tmp file, rename the file
	 * to remove the .tmp suffix.
	 */
	if (rename(pathtmp, path) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						pathtmp, path)));

	/*
	 * The basename of the file is what we return from pg_export_snapshot().
	 * It's already in path in a textual format and we know that the path
	 * starts with SNAPSHOT_EXPORT_DIR.  Skip over the prefix and the slash
	 * and pstrdup it so as not to return the address of a local variable.
	 */
	return pstrdup(path + strlen(SNAPSHOT_EXPORT_DIR) + 1);
}

/*
 * pg_export_snapshot
 *		SQL-callable wrapper for ExportSnapshot.
 */
Datum
pg_export_snapshot(PG_FUNCTION_ARGS)
{
	char	   *snapshotName;

	snapshotName = ExportSnapshot(((MVCCSnapshot) GetActiveSnapshot())->shared);
	PG_RETURN_TEXT_P(cstring_to_text(snapshotName));
}


/*
 * Parsing subroutines for ImportSnapshot: parse a line with the given
 * prefix followed by a value, and advance *s to the next line.  The
 * filename is provided for use in error messages.
 */
static int
parseIntFromText(const char *prefix, char **s, const char *filename)
{
	char	   *ptr = *s;
	int			prefixlen = strlen(prefix);
	int			val;

	if (strncmp(ptr, prefix, prefixlen) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr += prefixlen;
	if (sscanf(ptr, "%d", &val) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr = strchr(ptr, '\n');
	if (!ptr)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	*s = ptr + 1;
	return val;
}

static TransactionId
parseXidFromText(const char *prefix, char **s, const char *filename)
{
	char	   *ptr = *s;
	int			prefixlen = strlen(prefix);
	TransactionId val;

	if (strncmp(ptr, prefix, prefixlen) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr += prefixlen;
	if (sscanf(ptr, "%u", &val) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr = strchr(ptr, '\n');
	if (!ptr)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	*s = ptr + 1;
	return val;
}

static void
parseVxidFromText(const char *prefix, char **s, const char *filename,
				  VirtualTransactionId *vxid)
{
	char	   *ptr = *s;
	int			prefixlen = strlen(prefix);

	if (strncmp(ptr, prefix, prefixlen) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr += prefixlen;
	if (sscanf(ptr, "%d/%u", &vxid->procNumber, &vxid->localTransactionId) != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr = strchr(ptr, '\n');
	if (!ptr)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	*s = ptr + 1;
}

/*
 * ImportSnapshot
 *		Import a previously exported snapshot.  The argument should be a
 *		filename in SNAPSHOT_EXPORT_DIR.  Load the snapshot from that file.
 *		This is called by "SET TRANSACTION SNAPSHOT 'foo'".
 */
void
ImportSnapshot(const char *idstr)
{
	char		path[MAXPGPATH];
	FILE	   *f;
	struct stat stat_buf;
	char	   *filebuf;
	int			xcnt;
	int			i;
	VirtualTransactionId src_vxid;
	int			src_pid;
	Oid			src_dbid;
	int			src_isolevel;
	bool		src_readonly;
	MVCCSnapshotShared snapshot;

	/*
	 * Must be at top level of a fresh transaction.  Note in particular that
	 * we check we haven't acquired an XID --- if we have, it's conceivable
	 * that the snapshot would show it as not running, making for very screwy
	 * behavior.
	 */
	if (FirstSnapshotSet ||
		GetTopTransactionIdIfAny() != InvalidTransactionId ||
		IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("SET TRANSACTION SNAPSHOT must be called before any query")));

	/*
	 * If we are in read committed mode then the next query would execute with
	 * a new snapshot thus making this function call quite useless.
	 */
	if (!IsolationUsesXactSnapshot())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a snapshot-importing transaction must have isolation level SERIALIZABLE or REPEATABLE READ")));

	/*
	 * Verify the identifier: only 0-9, A-F and hyphens are allowed.  We do
	 * this mainly to prevent reading arbitrary files.
	 */
	if (strspn(idstr, "0123456789ABCDEF-") != strlen(idstr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid snapshot identifier: \"%s\"", idstr)));

	/* OK, read the file */
	snprintf(path, MAXPGPATH, SNAPSHOT_EXPORT_DIR "/%s", idstr);

	f = AllocateFile(path, PG_BINARY_R);
	if (!f)
	{
		/*
		 * If file is missing while identifier has a correct format, avoid
		 * system errors.
		 */
		if (errno == ENOENT)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("snapshot \"%s\" does not exist", idstr)));
		else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for reading: %m",
							path)));
	}

	/* get the size of the file so that we know how much memory we need */
	if (fstat(fileno(f), &stat_buf))
		elog(ERROR, "could not stat file \"%s\": %m", path);

	/* and read the file into a palloc'd string */
	filebuf = (char *) palloc(stat_buf.st_size + 1);
	if (fread(filebuf, stat_buf.st_size, 1, f) != 1)
		elog(ERROR, "could not read file \"%s\": %m", path);

	filebuf[stat_buf.st_size] = '\0';

	FreeFile(f);

	/*
	 * Construct a snapshot struct by parsing the file content.
	 */
	parseVxidFromText("vxid:", &filebuf, path, &src_vxid);
	src_pid = parseIntFromText("pid:", &filebuf, path);
	/* we abuse parseXidFromText a bit here ... */
	src_dbid = parseXidFromText("dbid:", &filebuf, path);
	src_isolevel = parseIntFromText("iso:", &filebuf, path);
	src_readonly = parseIntFromText("ro:", &filebuf, path);

	snapshot = AllocMVCCSnapshotShared();
	snapshot->xmin = parseXidFromText("xmin:", &filebuf, path);
	snapshot->xmax = parseXidFromText("xmax:", &filebuf, path);

	snapshot->xcnt = xcnt = parseIntFromText("xcnt:", &filebuf, path);

	/* sanity-check the xid count before palloc */
	if (xcnt < 0 || xcnt > GetMaxSnapshotXidCount())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", path)));

	snapshot->xip = (TransactionId *) palloc(xcnt * sizeof(TransactionId));
	for (i = 0; i < xcnt; i++)
		snapshot->xip[i] = parseXidFromText("xip:", &filebuf, path);

	snapshot->suboverflowed = parseIntFromText("sof:", &filebuf, path);

	if (!snapshot->suboverflowed)
	{
		snapshot->subxcnt = xcnt = parseIntFromText("sxcnt:", &filebuf, path);

		/* sanity-check the xid count before palloc */
		if (xcnt < 0 || xcnt > GetMaxSnapshotSubxidCount())
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid snapshot data in file \"%s\"", path)));

		snapshot->subxip = (TransactionId *) palloc(xcnt * sizeof(TransactionId));
		for (i = 0; i < xcnt; i++)
			snapshot->subxip[i] = parseXidFromText("sxp:", &filebuf, path);
	}
	else
	{
		snapshot->subxcnt = 0;
	}

	snapshot->takenDuringRecovery = parseIntFromText("rec:", &filebuf, path);
	snapshot->snapshotCsn = parseIntFromText("snapshotcsn:", &filebuf, path);

	snapshot->refcount = 1;
	valid_snapshots_push_out_of_order(snapshot);

	/*
	 * Do some additional sanity checking, just to protect ourselves.  We
	 * don't trouble to check the array elements, just the most critical
	 * fields.
	 */
	if (!VirtualTransactionIdIsValid(src_vxid) ||
		!OidIsValid(src_dbid) ||
		!TransactionIdIsNormal(snapshot->xmin) ||
		!TransactionIdIsNormal(snapshot->xmax))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", path)));

	/*
	 * If we're serializable, the source transaction must be too, otherwise
	 * predicate.c has problems (SxactGlobalXmin could go backwards).  Also, a
	 * non-read-only transaction can't adopt a snapshot from a read-only
	 * transaction, as predicate.c handles the cases very differently.
	 */
	if (IsolationIsSerializable())
	{
		if (src_isolevel != XACT_SERIALIZABLE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a serializable transaction cannot import a snapshot from a non-serializable transaction")));
		if (src_readonly && !XactReadOnly)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a non-read-only serializable transaction cannot import a snapshot from a read-only transaction")));
	}

	/*
	 * We cannot import a snapshot that was taken in a different database,
	 * because vacuum calculates OldestXmin on a per-database basis; so the
	 * source transaction's xmin doesn't protect us from data loss.  This
	 * restriction could be removed if the source transaction were to mark its
	 * xmin as being globally applicable.  But that would require some
	 * additional syntax, since that has to be known when the snapshot is
	 * initially taken.  (See pgsql-hackers discussion of 2011-10-21.)
	 */
	if (src_dbid != MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot import a snapshot from a different database")));

	/* OK, install the snapshot */
	SetTransactionSnapshot(snapshot, &src_vxid, src_pid, NULL);
}

/*
 * XactHasExportedSnapshots
 *		Test whether current transaction has exported any snapshots.
 */
bool
XactHasExportedSnapshots(void)
{
	return (exportedSnapshots != NIL);
}

/*
 * DeleteAllExportedSnapshotFiles
 *		Clean up any files that have been left behind by a crashed backend
 *		that had exported snapshots before it died.
 *
 * This should be called during database startup or crash recovery.
 */
void
DeleteAllExportedSnapshotFiles(void)
{
	char		buf[MAXPGPATH + sizeof(SNAPSHOT_EXPORT_DIR)];
	DIR		   *s_dir;
	struct dirent *s_de;

	/*
	 * Problems in reading the directory, or unlinking files, are reported at
	 * LOG level.  Since we're running in the startup process, ERROR level
	 * would prevent database start, and it's not important enough for that.
	 */
	s_dir = AllocateDir(SNAPSHOT_EXPORT_DIR);

	while ((s_de = ReadDirExtended(s_dir, SNAPSHOT_EXPORT_DIR, LOG)) != NULL)
	{
		if (strcmp(s_de->d_name, ".") == 0 ||
			strcmp(s_de->d_name, "..") == 0)
			continue;

		snprintf(buf, sizeof(buf), SNAPSHOT_EXPORT_DIR "/%s", s_de->d_name);

		if (unlink(buf) != 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", buf)));
	}

	FreeDir(s_dir);
}

/*
 * ThereAreNoPriorRegisteredSnapshots
 *		Are there any snapshots other than the current active snapshot?
 *
 * Don't use this to settle important decisions.  While zero registrations and
 * no ActiveSnapshot would confirm a certain idleness, the system makes no
 * guarantees about the significance of one registered snapshot.
 */
bool
ThereAreNoPriorRegisteredSnapshots(void)
{
	dlist_iter	iter;

	dlist_foreach(iter, &ValidSnapshots)
	{
		MVCCSnapshotShared cur =
			dlist_container(MVCCSnapshotSharedData, node, iter.cur);
		uint32		allowedcount = 0;

		if (FirstXactSnapshotRegistered)
		{
			Assert(CurrentSnapshotData.valid);
			if (cur == CurrentSnapshotData.shared)
				allowedcount++;
		}
		if (ActiveSnapshot && cur == ActiveSnapshot->as_snap.shared)
			allowedcount++;

		if (cur->refcount != allowedcount)
			return false;
	}

	return true;
}

/*
 * HaveRegisteredOrActiveSnapshot
 *		Is there any registered or active snapshot?
 *
 * NB: Unless pushed or active, the cached catalog snapshot will not cause
 * this function to return true. That allows this function to be used in
 * checks enforcing a longer-lived snapshot.
 */
bool
HaveRegisteredOrActiveSnapshot(void)
{
	if (ActiveSnapshot != NULL)
		return true;

	/*
	 * The catalog snapshot is in ValidSnapshots when valid, but can be
	 * removed at any time due to invalidation processing. If explicitly
	 * registered more than one snapshot has to be in ValidSnapshots.
	 */
	if (CatalogSnapshotData.valid &&
		CatalogSnapshotData.shared->refcount == 1 &&
		dlist_head_node(&ValidSnapshots) == &CatalogSnapshotData.shared->node &&
		dlist_tail_node(&ValidSnapshots) == &CatalogSnapshotData.shared->node)
	{
		return false;
	}

	return !dlist_is_empty(&ValidSnapshots);
}


/*
 * Setup a snapshot that replaces normal catalog snapshots that allows catalog
 * access to behave just like it did at a certain point in the past.
 *
 * Needed for logical decoding.
 */
void
SetupHistoricSnapshot(HistoricMVCCSnapshot historic_snapshot, HTAB *tuplecids)
{
	Assert(historic_snapshot != NULL);

	/* setup the timetravel snapshot */
	HistoricSnapshot = historic_snapshot;

	/* setup (cmin, cmax) lookup hash */
	tuplecid_data = tuplecids;
}


/*
 * Make catalog snapshots behave normally again.
 */
void
TeardownHistoricSnapshot(bool is_error)
{
	HistoricSnapshot = NULL;
	tuplecid_data = NULL;
}

bool
HistoricSnapshotActive(void)
{
	return HistoricSnapshot != NULL;
}

HTAB *
HistoricSnapshotGetTupleCids(void)
{
	Assert(HistoricSnapshotActive());
	return tuplecid_data;
}

/*
 * EstimateSnapshotSpace
 *		Returns the size needed to store the given snapshot.
 *
 * We are exporting only required fields from the Snapshot, stored in
 * SerializedSnapshotData.
 */
Size
EstimateSnapshotSpace(MVCCSnapshot snapshot)
{
	Size		size;

	Assert(snapshot->snapshot_type == SNAPSHOT_MVCC);

	/* We allocate any XID arrays needed in the same palloc block. */
	size = add_size(sizeof(SerializedSnapshotData),
					mul_size(snapshot->shared->xcnt, sizeof(TransactionId)));
	if (snapshot->shared->subxcnt > 0 &&
		(!snapshot->shared->suboverflowed || snapshot->shared->takenDuringRecovery))
		size = add_size(size,
						mul_size(snapshot->shared->subxcnt, sizeof(TransactionId)));

	return size;
}

/*
 * SerializeSnapshot
 *		Dumps the serialized snapshot (extracted from given snapshot) onto the
 *		memory location at start_address.
 */
void
SerializeSnapshot(MVCCSnapshot snapshot, char *start_address)
{
	SerializedSnapshotData serialized_snapshot;

	Assert(snapshot->shared->subxcnt >= 0);

	/* Copy all required fields */
	serialized_snapshot.xmin = snapshot->shared->xmin;
	serialized_snapshot.xmax = snapshot->shared->xmax;
	serialized_snapshot.xcnt = snapshot->shared->xcnt;
	serialized_snapshot.subxcnt = snapshot->shared->subxcnt;
	serialized_snapshot.suboverflowed = snapshot->shared->suboverflowed;
	serialized_snapshot.takenDuringRecovery = snapshot->shared->takenDuringRecovery;
	serialized_snapshot.snapshotCsn = snapshot->shared->snapshotCsn;
	serialized_snapshot.curcid = snapshot->curcid;

	/*
	 * Ignore the SubXID array if it has overflowed, unless the snapshot was
	 * taken during recovery - in that case, top-level XIDs are in subxip as
	 * well, and we mustn't lose them.
	 */
	if (serialized_snapshot.suboverflowed && !snapshot->shared->takenDuringRecovery)
		serialized_snapshot.subxcnt = 0;

	/* Copy struct to possibly-unaligned buffer */
	memcpy(start_address,
		   &serialized_snapshot, sizeof(SerializedSnapshotData));

	/* Copy XID array */
	if (snapshot->shared->xcnt > 0)
		memcpy((TransactionId *) (start_address +
								  sizeof(SerializedSnapshotData)),
			   snapshot->shared->xip, snapshot->shared->xcnt * sizeof(TransactionId));

	/*
	 * Copy SubXID array. Don't bother to copy it if it had overflowed,
	 * though, because it's not used anywhere in that case. Except if it's a
	 * snapshot taken during recovery; all the top-level XIDs are in subxip as
	 * well in that case, so we mustn't lose them.
	 */
	if (serialized_snapshot.subxcnt > 0)
	{
		Size		subxipoff = sizeof(SerializedSnapshotData) +
			snapshot->shared->xcnt * sizeof(TransactionId);

		memcpy((TransactionId *) (start_address + subxipoff),
			   snapshot->shared->subxip, snapshot->shared->subxcnt * sizeof(TransactionId));
	}
}

/*
 * RestoreSnapshot
 *		Restore a serialized snapshot from the specified address.
 *
 * The copy is palloc'd in TopTransactionContext and has initial refcounts set
 * to 0.  The returned snapshot is registered with the current resource owner.
 */
MVCCSnapshot
RestoreSnapshot(char *start_address)
{
	SerializedSnapshotData serialized_snapshot;
	Size		size;
	MVCCSnapshot snapshot;
	TransactionId *serialized_xids;

	memcpy(&serialized_snapshot, start_address,
		   sizeof(SerializedSnapshotData));
	serialized_xids = (TransactionId *)
		(start_address + sizeof(SerializedSnapshotData));

	/* We allocate any XID arrays needed in the same palloc block. */
	size = sizeof(MVCCSnapshotData)
		+ serialized_snapshot.xcnt * sizeof(TransactionId)
		+ serialized_snapshot.subxcnt * sizeof(TransactionId);
	Assert(serialized_snapshot.xcnt <= GetMaxSnapshotXidCount());
	Assert(serialized_snapshot.subxcnt <= GetMaxSnapshotSubxidCount());

	/* Copy all required fields */
	snapshot = (MVCCSnapshot) MemoryContextAlloc(TopTransactionContext, size);
	snapshot->snapshot_type = SNAPSHOT_MVCC;
	snapshot->kind = SNAPSHOT_REGISTERED;
	snapshot->shared = AllocMVCCSnapshotShared();
	snapshot->shared->xmin = serialized_snapshot.xmin;
	snapshot->shared->xmax = serialized_snapshot.xmax;
	snapshot->shared->xcnt = serialized_snapshot.xcnt;
	snapshot->shared->subxcnt = serialized_snapshot.subxcnt;
	snapshot->shared->suboverflowed = serialized_snapshot.suboverflowed;
	snapshot->shared->takenDuringRecovery = serialized_snapshot.takenDuringRecovery;
	snapshot->shared->snapshotCsn = serialized_snapshot.snapshotCsn;
	snapshot->shared->inprogress_cache = NULL;
	snapshot->shared->inprogress_cache_cxt = NULL;
	snapshot->shared->snapXactCompletionCount = 0;

	snapshot->shared->refcount = 1;
	valid_snapshots_push_out_of_order(snapshot->shared);

	snapshot->curcid = serialized_snapshot.curcid;

	/* Copy XIDs, if present. */
	if (serialized_snapshot.xcnt > 0)
	{
		memcpy(snapshot->shared->xip, serialized_xids,
			   serialized_snapshot.xcnt * sizeof(TransactionId));
	}

	/* Copy SubXIDs, if present. */
	if (serialized_snapshot.subxcnt > 0)
	{
		memcpy(snapshot->shared->subxip, serialized_xids + serialized_snapshot.xcnt,
			   serialized_snapshot.subxcnt * sizeof(TransactionId));
	}

	snapshot->valid = true;

	/* and tell resowner.c about it, just like RegisterSnapshot() */
	ResourceOwnerEnlarge(CurrentResourceOwner);
	ResourceOwnerRememberSnapshot(CurrentResourceOwner, (Snapshot) snapshot);

	return snapshot;
}

/*
 * Install a restored snapshot as the transaction snapshot.
 *
 * The second argument is of type void * so that snapmgr.h need not include
 * the declaration for PGPROC.
 */
void
RestoreTransactionSnapshot(MVCCSnapshot snapshot, void *source_pgproc)
{
	SetTransactionSnapshot(snapshot->shared, NULL, InvalidPid, source_pgproc);
}

/*
 * XidInMVCCSnapshot
 *		Is the given XID still-in-progress according to the snapshot?
 *
 * Note: GetMVCCSnapshotData never stores either top xid or subxids of our own
 * backend into a snapshot, so these xids will not be reported as "running" by
 * this function.  This is OK for current uses, because we always check
 * TransactionIdIsCurrentTransactionId first, except when it's known the XID
 * could not be ours anyway.
 */
bool
XidInMVCCSnapshot(TransactionId xid, MVCCSnapshotShared snapshot)
{
	/*
	 * Make a quick range check to eliminate most XIDs without looking at the
	 * xip arrays.  Note that this is OK even if we convert a subxact XID to
	 * its parent below, because a subxact with XID < xmin has surely also got
	 * a parent with XID < xmin, while one with XID >= xmax must belong to a
	 * parent that was not yet committed at the time of this snapshot.
	 */

	/* Any xid < xmin is not in-progress */
	if (TransactionIdPrecedes(xid, snapshot->xmin))
		return false;
	/* Any xid >= xmax is in-progress */
	if (TransactionIdFollowsOrEquals(xid, snapshot->xmax))
		return true;

	/*
	 * Snapshot information is stored slightly differently in snapshots taken
	 * during recovery.
	 */
	if (!snapshot->takenDuringRecovery)
	{
		/*
		 * If the snapshot contains full subxact data, the fastest way to
		 * check things is just to compare the given XID against both subxact
		 * XIDs and top-level XIDs.  If the snapshot overflowed, we have to
		 * use pg_subtrans to convert a subxact XID to its parent XID, but
		 * then we need only look at top-level XIDs not subxacts.
		 */
		if (!snapshot->suboverflowed)
		{
			/* we have full data, so search subxip */
			if (pg_lfind32(xid, snapshot->subxip, snapshot->subxcnt))
				return true;

			/* not there, fall through to search xip[] */
		}
		else
		{
			/*
			 * Snapshot overflowed, so convert xid to top-level.  This is safe
			 * because we eliminated too-old XIDs above.
			 */
			xid = SubTransGetTopmostTransaction(xid);

			/*
			 * If xid was indeed a subxact, we might now have an xid < xmin,
			 * so recheck to avoid an array scan.  No point in rechecking
			 * xmax.
			 */
			if (TransactionIdPrecedes(xid, snapshot->xmin))
				return false;
		}

		if (pg_lfind32(xid, snapshot->xip, snapshot->xcnt))
			return true;
	}
	else
	{
		XLogRecPtr	csn;
		bool		inprogress;
		uint64	   *cache_entry;
		uint64		cache_word = 0;

		/*
		 * Calculate the word and bit slot for the XID in the cache. We use an
		 * offset from xmax as the key instead of the XID directly, because
		 * the radix tree can compact away leading zeros and is thus more
		 * efficient with keys closer to 0.
		 */
		uint32		cache_idx = snapshot->xmax - xid;
		uint64		wordno = cache_idx / INPROGRESS_CACHE_XIDS_PER_WORD;
		uint64		slotno = (cache_idx % INPROGRESS_CACHE_XIDS_PER_WORD) * INPROGRESS_CACHE_BITS;

		if (snapshot->inprogress_cache)
		{
			cache_entry = inprogress_cache_find(snapshot->inprogress_cache, wordno);
			if (cache_entry)
			{
				cache_word = *cache_entry;
				if (INPROGRESS_CACHE_XID_IS_CACHED(cache_word, slotno))
					return INPROGRESS_CACHE_XID_IS_IN_PROGRESS(cache_word, slotno);
			}
		}
		else
		{
			MemoryContext save_cxt;

			save_cxt = MemoryContextSwitchTo(TopMemoryContext);

			if (snapshot->inprogress_cache_cxt == NULL)
				snapshot->inprogress_cache_cxt =
					AllocSetContextCreate(TopMemoryContext,
										  "snapshot inprogress cache context",
										  ALLOCSET_SMALL_SIZES);
			snapshot->inprogress_cache = inprogress_cache_create(snapshot->inprogress_cache_cxt);
			cache_entry = NULL;
			MemoryContextSwitchTo(save_cxt);
		}

		/* Not found in cache, look up the CSN */
		csn = CSNLogGetCSNByXid(xid);
		inprogress = (csn == InvalidXLogRecPtr || csn > snapshot->snapshotCsn);

		/* Update the cache word, and store it back to the radix tree */
		cache_word |= UINT64CONST(1) << slotno; /* cached */
		if (inprogress)
			cache_word |= UINT64CONST(1) << (slotno + 1);	/* in-progress */

		if (cache_entry)
			*cache_entry = cache_word;
		else
			inprogress_cache_set(snapshot->inprogress_cache, wordno, &cache_word);

		return inprogress;
	}

	return false;
}

/*
 * Allocate an MVCCSnapshotShared struct
 *
 * The 'xip' and 'subxip' arrays are allocated so that they can hold the max
 * number of XIDs. That's usually overkill, but it allows us to do the
 * allocation while not holding ProcArrayLock.
 *
 * MVCCSnapshotShared structs are kept in TopMemoryContext and refcounted.
 * The refcount is initially zero, the caller is expected to increment it.
 */
MVCCSnapshotShared
AllocMVCCSnapshotShared(void)
{
	MemoryContext save_cxt;
	MVCCSnapshotShared shared;
	size_t		size;
	char	   *p;

	/*
	 * To reduce alloc/free overhead in GetMVCCSnapshotData(), we have a
	 * single-element pool.
	 */
	if (spareSnapshotShared)
	{
		shared = spareSnapshotShared;
		spareSnapshotShared = NULL;
		return shared;
	}

	save_cxt = MemoryContextSwitchTo(TopMemoryContext);

	size = sizeof(MVCCSnapshotSharedData) +
		GetMaxSnapshotXidCount() * sizeof(TransactionId) +
		GetMaxSnapshotSubxidCount() * sizeof(TransactionId);
	p = palloc(size);

	shared = (MVCCSnapshotShared) p;
	p += sizeof(MVCCSnapshotSharedData);
	shared->xip = (TransactionId *) p;
	p += GetMaxSnapshotXidCount() * sizeof(TransactionId);
	shared->subxip = (TransactionId *) p;

	shared->snapXactCompletionCount = 0;
	shared->refcount = 0;
	shared->snapshotCsn = InvalidXLogRecPtr;
	shared->inprogress_cache = NULL;
	shared->inprogress_cache_cxt = NULL;

	MemoryContextSwitchTo(save_cxt);

	return shared;
}

/*
 * Decrement the refcount on an MVCCSnapshotShared struct, freeing it if it
 * reaches zero.
 */
static void
ReleaseMVCCSnapshotShared(MVCCSnapshotShared shared)
{
	Assert(shared->refcount > 0);
	shared->refcount--;

	if (shared->refcount == 0)
	{
		dlist_delete(&shared->node);
		if (shared != latestSnapshotShared)
			FreeMVCCSnapshotShared(shared);
	}
}

void
FreeMVCCSnapshotShared(MVCCSnapshotShared shared)
{
	Assert(shared->refcount == 0);

	if (shared->inprogress_cache)
	{
		inprogress_cache_free(shared->inprogress_cache);
		shared->inprogress_cache = NULL;
	}
	if (shared->inprogress_cache_cxt)
	{
		MemoryContextDelete(shared->inprogress_cache_cxt);
		shared->inprogress_cache_cxt = NULL;
	}

	if (spareSnapshotShared == NULL)
	{
		spareSnapshotShared = shared;
	}
	else
		pfree(shared);
}

/* ResourceOwner callbacks */

static void
ResOwnerReleaseSnapshot(Datum res)
{
	UnregisterSnapshotNoOwner((Snapshot) DatumGetPointer(res));
}


/* Helper functions to manipulate the ValidSnapshots list */

/* dlist_push_tail, with assertion that the list stays ordered by xmin */
static void
valid_snapshots_push_tail(MVCCSnapshotShared snapshot)
{
#ifdef USE_ASSERT_CHECKING
	if (!dlist_is_empty(&ValidSnapshots))
	{
		MVCCSnapshotShared tail =
			dlist_tail_element(MVCCSnapshotSharedData, node, &ValidSnapshots);

		Assert(TransactionIdFollowsOrEquals(snapshot->xmin, tail->xmin));
	}
#endif
	dlist_push_tail(&ValidSnapshots, &snapshot->node);
}

/*
 * Add an entry to the right position in the list, keeping it ordered by xmin.
 *
 * This is O(n), but that's OK because it's only used in rare occasions, when
 * the list is small.
 */
static void
valid_snapshots_push_out_of_order(MVCCSnapshotShared snapshot)
{
	dlist_iter	iter;

	dlist_foreach(iter, &ValidSnapshots)
	{
		MVCCSnapshotShared cur =
			dlist_container(MVCCSnapshotSharedData, node, iter.cur);

		if (TransactionIdFollowsOrEquals(snapshot->xmin, cur->xmin))
		{
			dlist_insert_after(&cur->node, &snapshot->node);
			return;
		}
	}
	dlist_push_tail(&ValidSnapshots, &snapshot->node);
}
