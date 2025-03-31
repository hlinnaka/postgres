/*-------------------------------------------------------------------------
 *
 * snapshot.h
 *	  POSTGRES snapshot definition
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/snapshot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include "access/xlogdefs.h"
#include "lib/ilist.h"


/*
 * The different snapshot types.  We use the SnapshotData union to represent
 * both "regular" (MVCC) snapshots and "special" snapshots that have non-MVCC
 * semantics.  The specific semantics of a snapshot are encoded by its type.
 *
 * The behaviour of each type of snapshot should be documented alongside its
 * enum value, best in terms that are not specific to an individual table AM.
 *
 * The reason the snapshot type rather than a callback as it used to be is
 * that that allows to use the same snapshot for different table AMs without
 * having one callback per AM.
 *
 * The executor deals with MVCC snapshots, but the table AM and some other
 * parts of the system also support the special snapshots.
 */
typedef enum SnapshotType
{
	/*-------------------------------------------------------------------------
	 * A tuple is visible iff the tuple is valid for the given MVCC snapshot.
	 *
	 * Here, we consider the effects of:
	 * - all transactions committed as of the time of the given snapshot
	 * - previous commands of this transaction
	 *
	 * Does _not_ include:
	 * - transactions shown as in-progress by the snapshot
	 * - transactions started after the snapshot was taken
	 * - changes made by the current command
	 * -------------------------------------------------------------------------
	 */
	SNAPSHOT_MVCC = 0,

	/*-------------------------------------------------------------------------
	 * A tuple is visible iff the tuple is valid "for itself".
	 *
	 * Here, we consider the effects of:
	 * - all committed transactions (as of the current instant)
	 * - previous commands of this transaction
	 * - changes made by the current command
	 *
	 * Does _not_ include:
	 * - in-progress transactions (as of the current instant)
	 * -------------------------------------------------------------------------
	 */
	SNAPSHOT_SELF,

	/*
	 * Any tuple is visible.
	 */
	SNAPSHOT_ANY,

	/*
	 * A tuple is visible iff the tuple is valid as a TOAST row.
	 */
	SNAPSHOT_TOAST,

	/*-------------------------------------------------------------------------
	 * A tuple is visible iff the tuple is valid including effects of open
	 * transactions.
	 *
	 * Here, we consider the effects of:
	 * - all committed and in-progress transactions (as of the current instant)
	 * - previous commands of this transaction
	 * - changes made by the current command
	 *
	 * This is essentially like SNAPSHOT_SELF as far as effects of the current
	 * transaction and committed/aborted xacts are concerned.  However, it
	 * also includes the effects of other xacts still in progress.
	 *
	 * A special hack is that when a snapshot of this type is used to
	 * determine tuple visibility, the passed-in snapshot struct is used as an
	 * output argument to return the xids of concurrent xacts that affected
	 * the tuple.  snapshot->xmin is set to the tuple's xmin if that is
	 * another transaction that's still in progress; or to
	 * InvalidTransactionId if the tuple's xmin is committed good, committed
	 * dead, or my own xact.  Similarly for snapshot->xmax and the tuple's
	 * xmax.  If the tuple was inserted speculatively, meaning that the
	 * inserter might still back down on the insertion without aborting the
	 * whole transaction, the associated token is also returned in
	 * snapshot->speculativeToken.  See also InitDirtySnapshot().
	 * -------------------------------------------------------------------------
	 */
	SNAPSHOT_DIRTY,

	/*
	 * A tuple is visible iff it follows the rules of SNAPSHOT_MVCC, but
	 * supports being called in timetravel context (for decoding catalog
	 * contents in the context of logical decoding).  A historic MVCC snapshot
	 * should only be used on catalog tables, as we only track XIDs that
	 * modify catalogs during logical decoding.
	 */
	SNAPSHOT_HISTORIC_MVCC,

	/*
	 * A tuple is visible iff the tuple might be visible to some transaction;
	 * false if it's surely dead to everyone, i.e., vacuumable.
	 *
	 * For visibility checks snapshot->min must have been set up with the xmin
	 * horizon to use.
	 */
	SNAPSHOT_NON_VACUUMABLE,
} SnapshotType;

typedef struct MVCCSnapshotSharedData *MVCCSnapshotShared;

typedef enum MVCCSnapshotKind
{
	SNAPSHOT_STATIC,
	SNAPSHOT_ACTIVE,
	SNAPSHOT_REGISTERED,
} MVCCSnapshotKind;

struct inprogress_cache_radix_tree; /* private to snapmgr.c */

/*
 * Struct representing a normal MVCC snapshot.
 *
 * MVCC snapshots come in two variants: those taken during recovery in hot
 * standby mode, and "normal" MVCC snapshots.  They are distinguished by
 * shared->takenDuringRecovery.
 */
typedef struct MVCCSnapshotData
{
	SnapshotType snapshot_type; /* type of snapshot, must be first */

	/*
	 * Most fields are in this separate struct which can be reused and shared
	 * between snapshots that only differ in the command ID.  It is reference
	 * counted separately.
	 */
	MVCCSnapshotShared shared;

	CommandId	curcid;			/* in my xact, CID < curcid are visible */

	/*
	 * Book-keeping information, used by the snapshot manager
	 */
	MVCCSnapshotKind kind;
	bool		valid;
} MVCCSnapshotData;

typedef struct MVCCSnapshotSharedData
{
	/*
	 * An MVCC snapshot can never see the effects of XIDs >= xmax. It can see
	 * the effects of all older XIDs except those listed in the snapshot. xmin
	 * is stored as an optimization to avoid needing to search the XID arrays
	 * for most tuples.
	 */
	TransactionId xmin;			/* all XID < xmin are visible to me */
	TransactionId xmax;			/* all XID >= xmax are invisible to me */

	/*
	 * xip contains the all xact IDs that are in progress, unless the snapshot
	 * was taken during recovery in which case it's empty.
	 *
	 * note: all ids in xip[] satisfy xmin <= xip[i] < xmax
	 */
	TransactionId *xip;
	uint32		xcnt;			/* # of xact ids in xip[] */

	/*
	 * subxip contains subxact IDs that are in progress (and other
	 * transactions that are in progress if taken during recovery).
	 *
	 * note: all ids in subxip[] are >= xmin, but we don't bother filtering
	 * out any that are >= xmax
	 */
	TransactionId *subxip;
	int32		subxcnt;		/* # of xact ids in subxip[] */
	bool		suboverflowed;	/* has the subxip array overflowed? */

	/*
	 * MVCC snapshots taken during recovery use this CSN instead of the xip
	 * and subxip arrays. Any transactions that committed at or before this
	 * LSN are considered as visible.
	 */
	XLogRecPtr	snapshotCsn;

	/*
	 * Cache of XIDs known to be running or not according to the snapshot.
	 * Used in snapshots taken during recovery.
	 */
	struct inprogress_cache_radix_tree *inprogress_cache;
	MemoryContext inprogress_cache_cxt;

	bool		takenDuringRecovery;	/* recovery-shaped snapshot? */

	/*
	 * The transaction completion count at the time GetMVCCSnapshotData()
	 * built this snapshot. Allows to avoid re-computing static snapshots when
	 * no transactions completed since the last GetMVCCSnapshotData().
	 */
	uint64		snapXactCompletionCount;

	uint32		refcount;
	dlist_node	node;			/* link in ValidSnapshots */
} MVCCSnapshotSharedData;

typedef struct MVCCSnapshotData *MVCCSnapshot;

#define InvalidMVCCSnapshot ((MVCCSnapshot) NULL)

/*
 * Struct representing a "historic" MVCC snapshot during logical decoding.
 * These are constructed by src/replication/logical/snapbuild.c.
 */
typedef struct HistoricMVCCSnapshotData
{
	SnapshotType snapshot_type; /* type of snapshot, must be first */

	/*
	 * xmin and xmax like in a normal MVCC snapshot.
	 */
	TransactionId xmin;			/* all XID < xmin are visible to me */
	TransactionId xmax;			/* all XID >= xmax are invisible to me */

	/*
	 * committed_xids contains *committed* transactions between xmin and xmax.
	 * (This is the inverse of 'xip' in normal MVCC snapshots, which contains
	 * all non-committed transactions.)  The array is sorted by XID to allow
	 * binary search.
	 *
	 * note: all ids in committed_xids[] satisfy xmin <= committed_xids[i] <
	 * xmax
	 */
	TransactionId *committed_xids;
	uint32		xcnt;			/* # of xact ids in committed_xids[] */

	/*
	 * curxip contains *all* xids assigned to the replayed transaction,
	 * including the toplevel xid.
	 */
	TransactionId *curxip;
	int32		curxcnt;		/* # of xact ids in curxip[] */

	CommandId	curcid;			/* in my xact, CID < curcid are visible */

	uint32		refcount;		/* refcount managed by snapbuild.c  */
	uint32		regd_count;		/* refcount registered with resource owners */

} HistoricMVCCSnapshotData;

typedef struct HistoricMVCCSnapshotData *HistoricMVCCSnapshot;

/*
 * Struct representing a special "snapshot" which sees all tuples as visible
 * if they are visible to anyone, i.e. if they are not vacuumable.
 * i.e. SNAPSHOT_NON_VACUUMABLE.
 */
typedef struct NonVacuumableSnapshotData
{
	SnapshotType snapshot_type; /* type of snapshot, must be first */

	/* This is used to determine whether row could be vacuumed. */
	struct GlobalVisState *vistest;
} NonVacuumableSnapshotData;

/*
 * Return values to the caller of HeapTupleSatisfyDirty.
 */
typedef struct DirtySnapshotData
{
	SnapshotType snapshot_type; /* type of snapshot, must be first */

	TransactionId xmin;
	TransactionId xmax;
	uint32		speculativeToken;
} DirtySnapshotData;

/*
 * Generic union representing all kind of possible snapshots.  Some have
 * type-specific structs.
 */
typedef union SnapshotData
{
	SnapshotType snapshot_type; /* type of snapshot */

	MVCCSnapshotData mvcc;
	DirtySnapshotData dirty;
	HistoricMVCCSnapshotData historic_mvcc;
	NonVacuumableSnapshotData nonvacuumable;
} SnapshotData;

typedef union SnapshotData *Snapshot;

#define InvalidSnapshot		((Snapshot) NULL)

#endif							/* SNAPSHOT_H */
