/*-------------------------------------------------------------------------
 *
 * aggpath.c
 *	  Routines to generate paths for processing GROUP BY and aggregates.
 *
 * XXX
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/aggpath.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "executor/nodeAgg.h"
#include "foreign/fdwapi.h"
#include "lib/knapsack.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

static bool is_degenerate_grouping(PlannerInfo *root);
static void create_degenerate_grouping_paths(PlannerInfo *root,
								 RelOptInfo *input_rel,
								 RelOptInfo *grouped_rel);
static void create_ordinary_grouping_paths(PlannerInfo *root,
							   RelOptInfo *input_rel,
							   RelOptInfo *grouped_rel,
							   const AggClauseCosts *agg_costs,
							   grouping_sets_data *gd,
							   GroupPathExtraData *extra,
							   RelOptInfo **partially_grouped_rel_p);
static bool can_partial_agg(PlannerInfo *root,
				const AggClauseCosts *agg_costs);
static void create_partitionwise_grouping_paths(PlannerInfo *root,
									RelOptInfo *input_rel,
									RelOptInfo *grouped_rel,
									RelOptInfo *partially_grouped_rel,
									const AggClauseCosts *agg_costs,
									grouping_sets_data *gd,
									PartitionwiseAggregateType patype,
									GroupPathExtraData *extra);
static bool group_by_has_partkey(RelOptInfo *input_rel,
					 List *targetList,
					 List *groupClause);
static RelOptInfo *create_partial_grouping_paths(PlannerInfo *root,
							  RelOptInfo *grouped_rel,
							  RelOptInfo *input_rel,
							  grouping_sets_data *gd,
							  GroupPathExtraData *extra,
							  bool force_rel_creation);
static void gather_grouping_paths(PlannerInfo *root, RelOptInfo *rel);
static void add_paths_to_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *grouped_rel,
						  RelOptInfo *partially_grouped_rel,
						  const AggClauseCosts *agg_costs,
						  grouping_sets_data *gd,
						  double dNumGroups,
						  GroupPathExtraData *extra);
static PathTarget *make_partial_grouping_target(PlannerInfo *root,
							 PathTarget *grouping_target,
							 Node *havingQual);
static void consider_groupingsets_paths(PlannerInfo *root,
							RelOptInfo *grouped_rel,
							Path *path,
							bool is_sorted,
							bool can_hash,
							grouping_sets_data *gd,
							const AggClauseCosts *agg_costs,
							double dNumGroups);

/*
 * Estimate number of groups produced by grouping clauses (1 if not grouping)
 *
 * path_rows: number of output rows from scan/join step
 * gd: grouping sets data including list of grouping sets and their clauses
 * target_list: target list containing group clause references
 *
 * If doing grouping sets, we also annotate the gsets data with the estimates
 * for each set and each individual rollup list, with a view to later
 * determining whether some combination of them could be hashed instead.
 */
static double
get_number_of_groups(PlannerInfo *root,
					 double path_rows,
					 grouping_sets_data *gd,
					 List *target_list)
{
	Query	   *parse = root->parse;
	double		dNumGroups;

	if (parse->groupClause)
	{
		List	   *groupExprs;

		if (parse->groupingSets)
		{
			/* Add up the estimates for each grouping set */
			ListCell   *lc;
			ListCell   *lc2;

			Assert(gd);			/* keep Coverity happy */

			dNumGroups = 0;

			foreach(lc, gd->rollups)
			{
				RollupData *rollup = lfirst_node(RollupData, lc);
				ListCell   *lc;

				groupExprs = get_sortgrouplist_exprs(rollup->groupClause,
													 target_list);

				rollup->numGroups = 0.0;

				forboth(lc, rollup->gsets, lc2, rollup->gsets_data)
				{
					List	   *gset = (List *) lfirst(lc);
					GroupingSetData *gs = lfirst_node(GroupingSetData, lc2);
					double		numGroups = estimate_num_groups(root,
																groupExprs,
																path_rows,
																&gset);

					gs->numGroups = numGroups;
					rollup->numGroups += numGroups;
				}

				dNumGroups += rollup->numGroups;
			}

			if (gd->hash_sets_idx)
			{
				ListCell   *lc;

				gd->dNumHashGroups = 0;

				groupExprs = get_sortgrouplist_exprs(parse->groupClause,
													 target_list);

				forboth(lc, gd->hash_sets_idx, lc2, gd->unsortable_sets)
				{
					List	   *gset = (List *) lfirst(lc);
					GroupingSetData *gs = lfirst_node(GroupingSetData, lc2);
					double		numGroups = estimate_num_groups(root,
																groupExprs,
																path_rows,
																&gset);

					gs->numGroups = numGroups;
					gd->dNumHashGroups += numGroups;
				}

				dNumGroups += gd->dNumHashGroups;
			}
		}
		else
		{
			/* Plain GROUP BY */
			groupExprs = get_sortgrouplist_exprs(parse->groupClause,
												 target_list);

			dNumGroups = estimate_num_groups(root, groupExprs, path_rows,
											 NULL);
		}
	}
	else if (parse->groupingSets)
	{
		/* Empty grouping sets ... one result row for each one */
		dNumGroups = list_length(parse->groupingSets);
	}
	else if (parse->hasAggs || root->hasHavingQual)
	{
		/* Plain aggregation, one result row */
		dNumGroups = 1;
	}
	else
	{
		/* Not grouping */
		dNumGroups = 1;
	}

	return dNumGroups;
}

/*
 * estimate_hashagg_tablesize
 *	  estimate the number of bytes that a hash aggregate hashtable will
 *	  require based on the agg_costs, path width and dNumGroups.
 *
 * XXX this may be over-estimating the size now that hashagg knows to omit
 * unneeded columns from the hashtable. Also for mixed-mode grouping sets,
 * grouping columns not in the hashed set are counted here even though hashagg
 * won't store them. Is this a problem?
 */
static Size
estimate_hashagg_tablesize(Path *path, const AggClauseCosts *agg_costs,
						   double dNumGroups)
{
	Size		hashentrysize;

	/* Estimate per-hash-entry space at tuple width... */
	hashentrysize = MAXALIGN(path->pathtarget->width) +
		MAXALIGN(SizeofMinimalTupleHeader);

	/* plus space for pass-by-ref transition values... */
	hashentrysize += agg_costs->transitionSpace;
	/* plus the per-hash-entry overhead */
	hashentrysize += hash_agg_entry_size(agg_costs->numAggs);

	/*
	 * Note that this disregards the effect of fill-factor and growth policy
	 * of the hash-table. That's probably ok, given default the default
	 * fill-factor is relatively high. It'd be hard to meaningfully factor in
	 * "double-in-size" growth policies here.
	 */
	return hashentrysize * dNumGroups;
}

/*
 * create_grouping_paths
 *
 * Build a new upperrel containing Paths for grouping and/or aggregation.
 * Along the way, we also build an upperrel for Paths which are partially
 * grouped and/or aggregated.  A partially grouped and/or aggregated path
 * needs a FinalizeAggregate node to complete the aggregation.  Currently,
 * the only partially grouped paths we build are also partial paths; that
 * is, they need a Gather and then a FinalizeAggregate.
 *
 * input_rel: contains the source-data Paths
 * target: the pathtarget for the result Paths to compute
 * agg_costs: cost info about all aggregates in query (in AGGSPLIT_SIMPLE mode)
 * gd: grouping sets data including list of grouping sets and their clauses
 *
 * Note: all Paths in input_rel are expected to return the target computed
 * by make_group_input_target.
 */
void
create_grouping_paths(PlannerInfo *root,
					  RelOptInfo *input_rel,
					  RelOptInfo *grouped_rel,
					  PathTarget *target,
					  bool target_parallel_safe,
					  const AggClauseCosts *agg_costs,
					  struct grouping_sets_data *gd)
{
	Query	   *parse = root->parse;
	RelOptInfo *partially_grouped_rel;

	/*
	 * Create either paths for a degenerate grouping or paths for ordinary
	 * grouping, as appropriate.
	 */
	if (is_degenerate_grouping(root))
		create_degenerate_grouping_paths(root, input_rel, grouped_rel);
	else
	{
		int			flags = 0;
		GroupPathExtraData extra;

		/*
		 * Determine whether it's possible to perform sort-based
		 * implementations of grouping.  (Note that if groupClause is empty,
		 * grouping_is_sortable() is trivially true, and all the
		 * pathkeys_contained_in() tests will succeed too, so that we'll
		 * consider every surviving input path.)
		 *
		 * If we have grouping sets, we might be able to sort some but not all
		 * of them; in this case, we need can_sort to be true as long as we
		 * must consider any sorted-input plan.
		 */
		if ((gd && gd->rollups != NIL)
			|| grouping_is_sortable(parse->groupClause))
			flags |= GROUPING_CAN_USE_SORT;

		/*
		 * Determine whether we should consider hash-based implementations of
		 * grouping.
		 *
		 * Hashed aggregation only applies if we're grouping. If we have
		 * grouping sets, some groups might be hashable but others not; in
		 * this case we set can_hash true as long as there is nothing globally
		 * preventing us from hashing (and we should therefore consider plans
		 * with hashes).
		 *
		 * Executor doesn't support hashed aggregation with DISTINCT or ORDER
		 * BY aggregates.  (Doing so would imply storing *all* the input
		 * values in the hash table, and/or running many sorts in parallel,
		 * either of which seems like a certain loser.)  We similarly don't
		 * support ordered-set aggregates in hashed aggregation, but that case
		 * is also included in the numOrderedAggs count.
		 *
		 * Note: grouping_is_hashable() is much more expensive to check than
		 * the other gating conditions, so we want to do it last.
		 */
		if ((parse->groupClause != NIL &&
			 agg_costs->numOrderedAggs == 0 &&
			 (gd ? gd->any_hashable : grouping_is_hashable(parse->groupClause))))
			flags |= GROUPING_CAN_USE_HASH;

		/*
		 * Determine whether partial aggregation is possible.
		 */
		if (can_partial_agg(root, agg_costs))
			flags |= GROUPING_CAN_PARTIAL_AGG;

		extra.flags = flags;
		extra.target_parallel_safe = target_parallel_safe;
		extra.havingQual = parse->havingQual;
		extra.targetList = parse->targetList;
		extra.partial_costs_set = false;

		/*
		 * Determine whether partitionwise aggregation is in theory possible.
		 * It can be disabled by the user, and for now, we don't try to
		 * support grouping sets.  create_ordinary_grouping_paths() will check
		 * additional conditions, such as whether input_rel is partitioned.
		 */
		if (enable_partitionwise_aggregate && !parse->groupingSets)
			extra.patype = PARTITIONWISE_AGGREGATE_FULL;
		else
			extra.patype = PARTITIONWISE_AGGREGATE_NONE;

		create_ordinary_grouping_paths(root, input_rel, grouped_rel,
									   agg_costs, gd, &extra,
									   &partially_grouped_rel);
	}

	set_cheapest(grouped_rel);
}

/*
 * is_degenerate_grouping
 *
 * A degenerate grouping is one in which the query has a HAVING qual and/or
 * grouping sets, but no aggregates and no GROUP BY (which implies that the
 * grouping sets are all empty).
 */
static bool
is_degenerate_grouping(PlannerInfo *root)
{
	Query	   *parse = root->parse;

	return (root->hasHavingQual || parse->groupingSets) &&
		!parse->hasAggs && parse->groupClause == NIL;
}

/*
 * create_degenerate_grouping_paths
 *
 * When the grouping is degenerate (see is_degenerate_grouping), we are
 * supposed to emit either zero or one row for each grouping set depending on
 * whether HAVING succeeds.  Furthermore, there cannot be any variables in
 * either HAVING or the targetlist, so we actually do not need the FROM table
 * at all! We can just throw away the plan-so-far and generate a Result node.
 * This is a sufficiently unusual corner case that it's not worth contorting
 * the structure of this module to avoid having to generate the earlier paths
 * in the first place.
 */
static void
create_degenerate_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
								 RelOptInfo *grouped_rel)
{
	Query	   *parse = root->parse;
	int			nrows;
	Path	   *path;

	nrows = list_length(parse->groupingSets);
	if (nrows > 1)
	{
		/*
		 * Doesn't seem worthwhile writing code to cons up a generate_series
		 * or a values scan to emit multiple rows. Instead just make N clones
		 * and append them.  (With a volatile HAVING clause, this means you
		 * might get between 0 and N output rows. Offhand I think that's
		 * desired.)
		 */
		List	   *paths = NIL;

		while (--nrows >= 0)
		{
			path = (Path *)
				create_result_path(root, grouped_rel,
								   grouped_rel->reltarget,
								   (List *) parse->havingQual);
			paths = lappend(paths, path);
		}
		path = (Path *)
			create_append_path(root,
							   grouped_rel,
							   paths,
							   NIL,
							   NULL,
							   0,
							   false,
							   NIL,
							   -1);
	}
	else
	{
		/* No grouping sets, or just one, so one output row */
		path = (Path *)
			create_result_path(root, grouped_rel,
							   grouped_rel->reltarget,
							   (List *) parse->havingQual);
	}

	add_path(grouped_rel, path);
}

/*
 * create_ordinary_grouping_paths
 *
 * Create grouping paths for the ordinary (that is, non-degenerate) case.
 *
 * We need to consider sorted and hashed aggregation in the same function,
 * because otherwise (1) it would be harder to throw an appropriate error
 * message if neither way works, and (2) we should not allow hashtable size
 * considerations to dissuade us from using hashing if sorting is not possible.
 *
 * *partially_grouped_rel_p will be set to the partially grouped rel which this
 * function creates, or to NULL if it doesn't create one.
 */
static void
create_ordinary_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
							   RelOptInfo *grouped_rel,
							   const AggClauseCosts *agg_costs,
							   grouping_sets_data *gd,
							   GroupPathExtraData *extra,
							   RelOptInfo **partially_grouped_rel_p)
{
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	RelOptInfo *partially_grouped_rel = NULL;
	double		dNumGroups;
	PartitionwiseAggregateType patype = PARTITIONWISE_AGGREGATE_NONE;

	/*
	 * If this is the topmost grouping relation or if the parent relation is
	 * doing some form of partitionwise aggregation, then we may be able to do
	 * it at this level also.  However, if the input relation is not
	 * partitioned, partitionwise aggregate is impossible, and if it is dummy,
	 * partitionwise aggregate is pointless.
	 */
	if (extra->patype != PARTITIONWISE_AGGREGATE_NONE &&
		input_rel->part_scheme && input_rel->part_rels &&
		!IS_DUMMY_REL(input_rel))
	{
		/*
		 * If this is the topmost relation or if the parent relation is doing
		 * full partitionwise aggregation, then we can do full partitionwise
		 * aggregation provided that the GROUP BY clause contains all of the
		 * partitioning columns at this level.  Otherwise, we can do at most
		 * partial partitionwise aggregation.  But if partial aggregation is
		 * not supported in general then we can't use it for partitionwise
		 * aggregation either.
		 */
		if (extra->patype == PARTITIONWISE_AGGREGATE_FULL &&
			group_by_has_partkey(input_rel, extra->targetList,
								 root->parse->groupClause))
			patype = PARTITIONWISE_AGGREGATE_FULL;
		else if ((extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0)
			patype = PARTITIONWISE_AGGREGATE_PARTIAL;
		else
			patype = PARTITIONWISE_AGGREGATE_NONE;
	}

	/*
	 * Before generating paths for grouped_rel, we first generate any possible
	 * partially grouped paths; that way, later code can easily consider both
	 * parallel and non-parallel approaches to grouping.
	 */
	if ((extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0)
	{
		bool		force_rel_creation;

		/*
		 * If we're doing partitionwise aggregation at this level, force
		 * creation of a partially_grouped_rel so we can add partitionwise
		 * paths to it.
		 */
		force_rel_creation = (patype == PARTITIONWISE_AGGREGATE_PARTIAL);

		partially_grouped_rel =
			create_partial_grouping_paths(root,
										  grouped_rel,
										  input_rel,
										  gd,
										  extra,
										  force_rel_creation);
	}

	/* Set out parameter. */
	*partially_grouped_rel_p = partially_grouped_rel;

	/* Apply partitionwise aggregation technique, if possible. */
	if (patype != PARTITIONWISE_AGGREGATE_NONE)
		create_partitionwise_grouping_paths(root, input_rel, grouped_rel,
											partially_grouped_rel, agg_costs,
											gd, patype, extra);

	/* If we are doing partial aggregation only, return. */
	if (extra->patype == PARTITIONWISE_AGGREGATE_PARTIAL)
	{
		Assert(partially_grouped_rel);

		if (partially_grouped_rel->pathlist)
			set_cheapest(partially_grouped_rel);

		return;
	}

	/* Gather any partially grouped partial paths. */
	if (partially_grouped_rel && partially_grouped_rel->partial_pathlist)
	{
		gather_grouping_paths(root, partially_grouped_rel);
		set_cheapest(partially_grouped_rel);
	}

	/*
	 * Estimate number of groups.
	 */
	dNumGroups = get_number_of_groups(root,
									  cheapest_path->rows,
									  gd,
									  extra->targetList);

	/* Build final grouping paths */
	add_paths_to_grouping_rel(root, input_rel, grouped_rel,
							  partially_grouped_rel, agg_costs, gd,
							  dNumGroups, extra);

	/* Give a helpful error if we failed to find any implementation */
	if (grouped_rel->pathlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement GROUP BY"),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (grouped_rel->fdwroutine &&
		grouped_rel->fdwroutine->GetForeignUpperPaths)
		grouped_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_GROUP_AGG,
													  input_rel, grouped_rel,
													  extra);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_GROUP_AGG,
									input_rel, grouped_rel,
									extra);
}

/*
 * add_paths_to_grouping_rel
 *
 * Add non-partial paths to grouping relation.
 */
static void
add_paths_to_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *grouped_rel,
						  RelOptInfo *partially_grouped_rel,
						  const AggClauseCosts *agg_costs,
						  grouping_sets_data *gd, double dNumGroups,
						  GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	ListCell   *lc;
	bool		can_hash = (extra->flags & GROUPING_CAN_USE_HASH) != 0;
	bool		can_sort = (extra->flags & GROUPING_CAN_USE_SORT) != 0;
	List	   *havingQual = (List *) extra->havingQual;
	AggClauseCosts *agg_final_costs = &extra->agg_final_costs;

	if (can_sort)
	{
		/*
		 * Use any available suitably-sorted path as input, and also consider
		 * sorting the cheapest-total path.
		 */
		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);
			bool		is_sorted;

			is_sorted = pathkeys_contained_in(root->group_pathkeys,
											  path->pathkeys);
			if (path == cheapest_path || is_sorted)
			{
				/* Sort the cheapest-total path if it isn't already sorted */
				if (!is_sorted)
					path = (Path *) create_sort_path(root,
													 grouped_rel,
													 path,
													 root->group_pathkeys,
													 -1.0);

				/* Now decide what to stick atop it */
				if (parse->groupingSets)
				{
					consider_groupingsets_paths(root, grouped_rel,
												path, true, can_hash,
												gd, agg_costs, dNumGroups);
				}
				else if (parse->hasAggs)
				{
					/*
					 * We have aggregation, possibly with plain GROUP BY. Make
					 * an AggPath.
					 */
					add_path(grouped_rel, (Path *)
							 create_agg_path(root,
											 grouped_rel,
											 path,
											 grouped_rel->reltarget,
											 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
											 AGGSPLIT_SIMPLE,
											 parse->groupClause,
											 havingQual,
											 agg_costs,
											 dNumGroups));
				}
				else if (parse->groupClause)
				{
					/*
					 * We have GROUP BY without aggregation or grouping sets.
					 * Make a GroupPath.
					 */
					add_path(grouped_rel, (Path *)
							 create_group_path(root,
											   grouped_rel,
											   path,
											   parse->groupClause,
											   havingQual,
											   dNumGroups));
				}
				else
				{
					/* Other cases should have been handled above */
					Assert(false);
				}
			}
		}

		/*
		 * Instead of operating directly on the input relation, we can
		 * consider finalizing a partially aggregated path.
		 */
		if (partially_grouped_rel != NULL)
		{
			foreach(lc, partially_grouped_rel->pathlist)
			{
				Path	   *path = (Path *) lfirst(lc);

				/*
				 * Insert a Sort node, if required.  But there's no point in
				 * sorting anything but the cheapest path.
				 */
				if (!pathkeys_contained_in(root->group_pathkeys, path->pathkeys))
				{
					if (path != partially_grouped_rel->cheapest_total_path)
						continue;
					path = (Path *) create_sort_path(root,
													 grouped_rel,
													 path,
													 root->group_pathkeys,
													 -1.0);
				}

				if (parse->hasAggs)
					add_path(grouped_rel, (Path *)
							 create_agg_path(root,
											 grouped_rel,
											 path,
											 grouped_rel->reltarget,
											 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
											 AGGSPLIT_FINAL_DESERIAL,
											 parse->groupClause,
											 havingQual,
											 agg_final_costs,
											 dNumGroups));
				else
					add_path(grouped_rel, (Path *)
							 create_group_path(root,
											   grouped_rel,
											   path,
											   parse->groupClause,
											   havingQual,
											   dNumGroups));
			}
		}
	}

	if (can_hash)
	{
		Size		hashaggtablesize;

		if (parse->groupingSets)
		{
			/*
			 * Try for a hash-only groupingsets path over unsorted input.
			 */
			consider_groupingsets_paths(root, grouped_rel,
										cheapest_path, false, true,
										gd, agg_costs, dNumGroups);
		}
		else
		{
			hashaggtablesize = estimate_hashagg_tablesize(cheapest_path,
														  agg_costs,
														  dNumGroups);

			/*
			 * Provided that the estimated size of the hashtable does not
			 * exceed work_mem, we'll generate a HashAgg Path, although if we
			 * were unable to sort above, then we'd better generate a Path, so
			 * that we at least have one.
			 */
			if (hashaggtablesize < work_mem * 1024L ||
				grouped_rel->pathlist == NIL)
			{
				/*
				 * We just need an Agg over the cheapest-total input path,
				 * since input order won't matter.
				 */
				add_path(grouped_rel, (Path *)
						 create_agg_path(root, grouped_rel,
										 cheapest_path,
										 grouped_rel->reltarget,
										 AGG_HASHED,
										 AGGSPLIT_SIMPLE,
										 parse->groupClause,
										 havingQual,
										 agg_costs,
										 dNumGroups));
			}
		}

		/*
		 * Generate a Finalize HashAgg Path atop of the cheapest partially
		 * grouped path, assuming there is one. Once again, we'll only do this
		 * if it looks as though the hash table won't exceed work_mem.
		 */
		if (partially_grouped_rel && partially_grouped_rel->pathlist)
		{
			Path	   *path = partially_grouped_rel->cheapest_total_path;

			hashaggtablesize = estimate_hashagg_tablesize(path,
														  agg_final_costs,
														  dNumGroups);

			if (hashaggtablesize < work_mem * 1024L)
				add_path(grouped_rel, (Path *)
						 create_agg_path(root,
										 grouped_rel,
										 path,
										 grouped_rel->reltarget,
										 AGG_HASHED,
										 AGGSPLIT_FINAL_DESERIAL,
										 parse->groupClause,
										 havingQual,
										 agg_final_costs,
										 dNumGroups));
		}
	}

	/*
	 * When partitionwise aggregate is used, we might have fully aggregated
	 * paths in the partial pathlist, because add_paths_to_append_rel() will
	 * consider a path for grouped_rel consisting of a Parallel Append of
	 * non-partial paths from each child.
	 */
	if (grouped_rel->partial_pathlist != NIL)
		gather_grouping_paths(root, grouped_rel);
}

/*
 * can_partial_agg
 *
 * Determines whether or not partial grouping and/or aggregation is possible.
 * Returns true when possible, false otherwise.
 */
static bool
can_partial_agg(PlannerInfo *root, const AggClauseCosts *agg_costs)
{
	Query	   *parse = root->parse;

	if (!parse->hasAggs && parse->groupClause == NIL)
	{
		/*
		 * We don't know how to do parallel aggregation unless we have either
		 * some aggregates or a grouping clause.
		 */
		return false;
	}
	else if (parse->groupingSets)
	{
		/* We don't know how to do grouping sets in parallel. */
		return false;
	}
	else if (agg_costs->hasNonPartial || agg_costs->hasNonSerial)
	{
		/* Insufficient support for partial mode. */
		return false;
	}

	/* Everything looks good. */
	return true;
}


/*
 * make_partial_grouping_target
 *	  Generate appropriate PathTarget for output of partial aggregate
 *	  (or partial grouping, if there are no aggregates) nodes.
 *
 * A partial aggregation node needs to emit all the same aggregates that
 * a regular aggregation node would, plus any aggregates used in HAVING;
 * except that the Aggref nodes should be marked as partial aggregates.
 *
 * In addition, we'd better emit any Vars and PlaceholderVars that are
 * used outside of Aggrefs in the aggregation tlist and HAVING.  (Presumably,
 * these would be Vars that are grouped by or used in grouping expressions.)
 *
 * grouping_target is the tlist to be emitted by the topmost aggregation step.
 * havingQual represents the HAVING clause.
 */
static PathTarget *
make_partial_grouping_target(PlannerInfo *root,
							 PathTarget *grouping_target,
							 Node *havingQual)
{
	Query	   *parse = root->parse;
	PathTarget *partial_target;
	List	   *non_group_cols;
	List	   *non_group_exprs;
	int			i;
	ListCell   *lc;

	partial_target = create_empty_pathtarget();
	non_group_cols = NIL;

	i = 0;
	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);

		if (sgref && parse->groupClause &&
			get_sortgroupref_clause_noerr(sgref, parse->groupClause) != NULL)
		{
			/*
			 * It's a grouping column, so add it to the partial_target as-is.
			 * (This allows the upper agg step to repeat the grouping calcs.)
			 */
			add_column_to_pathtarget(partial_target, expr, sgref);
		}
		else
		{
			/*
			 * Non-grouping column, so just remember the expression for later
			 * call to pull_var_clause.
			 */
			non_group_cols = lappend(non_group_cols, expr);
		}

		i++;
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars/Aggrefs it uses, too.
	 */
	if (havingQual)
		non_group_cols = lappend(non_group_cols, havingQual);

	/*
	 * Pull out all the Vars, PlaceHolderVars, and Aggrefs mentioned in
	 * non-group cols (plus HAVING), and add them to the partial_target if not
	 * already present.  (An expression used directly as a GROUP BY item will
	 * be present already.)  Note this includes Vars used in resjunk items, so
	 * we are covering the needs of ORDER BY and window specifications.
	 */
	non_group_exprs = pull_var_clause((Node *) non_group_cols,
									  PVC_INCLUDE_AGGREGATES |
									  PVC_RECURSE_WINDOWFUNCS |
									  PVC_INCLUDE_PLACEHOLDERS);

	add_new_columns_to_pathtarget(partial_target, non_group_exprs);

	/*
	 * Adjust Aggrefs to put them in partial mode.  At this point all Aggrefs
	 * are at the top level of the target list, so we can just scan the list
	 * rather than recursing through the expression trees.
	 */
	foreach(lc, partial_target->exprs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(lc);

		if (IsA(aggref, Aggref))
		{
			Aggref	   *newaggref;

			/*
			 * We shouldn't need to copy the substructure of the Aggref node,
			 * but flat-copy the node itself to avoid damaging other trees.
			 */
			newaggref = makeNode(Aggref);
			memcpy(newaggref, aggref, sizeof(Aggref));

			/* For now, assume serialization is required */
			mark_partial_aggref(newaggref, AGGSPLIT_INITIAL_SERIAL);

			lfirst(lc) = newaggref;
		}
	}

	/* clean up cruft */
	list_free(non_group_exprs);
	list_free(non_group_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, partial_target);
}

/*
 * create_partial_grouping_paths
 *
 * Create a new upper relation representing the result of partial aggregation
 * and populate it with appropriate paths.  Note that we don't finalize the
 * lists of paths here, so the caller can add additional partial or non-partial
 * paths and must afterward call gather_grouping_paths and set_cheapest on
 * the returned upper relation.
 *
 * All paths for this new upper relation -- both partial and non-partial --
 * have been partially aggregated but require a subsequent FinalizeAggregate
 * step.
 *
 * NB: This function is allowed to return NULL if it determines that there is
 * no real need to create a new RelOptInfo.
 */
static RelOptInfo *
create_partial_grouping_paths(PlannerInfo *root,
							  RelOptInfo *grouped_rel,
							  RelOptInfo *input_rel,
							  grouping_sets_data *gd,
							  GroupPathExtraData *extra,
							  bool force_rel_creation)
{
	Query	   *parse = root->parse;
	RelOptInfo *partially_grouped_rel;
	AggClauseCosts *agg_partial_costs = &extra->agg_partial_costs;
	AggClauseCosts *agg_final_costs = &extra->agg_final_costs;
	Path	   *cheapest_partial_path = NULL;
	Path	   *cheapest_total_path = NULL;
	double		dNumPartialGroups = 0;
	double		dNumPartialPartialGroups = 0;
	ListCell   *lc;
	bool		can_hash = (extra->flags & GROUPING_CAN_USE_HASH) != 0;
	bool		can_sort = (extra->flags & GROUPING_CAN_USE_SORT) != 0;

	/*
	 * Consider whether we should generate partially aggregated non-partial
	 * paths.  We can only do this if we have a non-partial path, and only if
	 * the parent of the input rel is performing partial partitionwise
	 * aggregation.  (Note that extra->patype is the type of partitionwise
	 * aggregation being used at the parent level, not this level.)
	 */
	if (input_rel->pathlist != NIL &&
		extra->patype == PARTITIONWISE_AGGREGATE_PARTIAL)
		cheapest_total_path = input_rel->cheapest_total_path;

	/*
	 * If parallelism is possible for grouped_rel, then we should consider
	 * generating partially-grouped partial paths.  However, if the input rel
	 * has no partial paths, then we can't.
	 */
	if (grouped_rel->consider_parallel && input_rel->partial_pathlist != NIL)
		cheapest_partial_path = linitial(input_rel->partial_pathlist);

	/*
	 * If we can't partially aggregate partial paths, and we can't partially
	 * aggregate non-partial paths, then don't bother creating the new
	 * RelOptInfo at all, unless the caller specified force_rel_creation.
	 */
	if (cheapest_total_path == NULL &&
		cheapest_partial_path == NULL &&
		!force_rel_creation)
		return NULL;

	/*
	 * Build a new upper relation to represent the result of partially
	 * aggregating the rows from the input relation.
	 */
	partially_grouped_rel = fetch_upper_rel(root,
											UPPERREL_PARTIAL_GROUP_AGG,
											grouped_rel->relids);
	partially_grouped_rel->consider_parallel =
		grouped_rel->consider_parallel;
	partially_grouped_rel->reloptkind = grouped_rel->reloptkind;
	partially_grouped_rel->serverid = grouped_rel->serverid;
	partially_grouped_rel->userid = grouped_rel->userid;
	partially_grouped_rel->useridiscurrent = grouped_rel->useridiscurrent;
	partially_grouped_rel->fdwroutine = grouped_rel->fdwroutine;

	/*
	 * Build target list for partial aggregate paths.  These paths cannot just
	 * emit the same tlist as regular aggregate paths, because (1) we must
	 * include Vars and Aggrefs needed in HAVING, which might not appear in
	 * the result tlist, and (2) the Aggrefs must be set in partial mode.
	 */
	partially_grouped_rel->reltarget =
		make_partial_grouping_target(root, grouped_rel->reltarget,
									 extra->havingQual);

	if (!extra->partial_costs_set)
	{
		/*
		 * Collect statistics about aggregates for estimating costs of
		 * performing aggregation in parallel.
		 */
		MemSet(agg_partial_costs, 0, sizeof(AggClauseCosts));
		MemSet(agg_final_costs, 0, sizeof(AggClauseCosts));
		if (parse->hasAggs)
		{
			List	   *partial_target_exprs;

			/* partial phase */
			partial_target_exprs = partially_grouped_rel->reltarget->exprs;
			get_agg_clause_costs(root, (Node *) partial_target_exprs,
								 AGGSPLIT_INITIAL_SERIAL,
								 agg_partial_costs);

			/* final phase */
			get_agg_clause_costs(root, (Node *) grouped_rel->reltarget->exprs,
								 AGGSPLIT_FINAL_DESERIAL,
								 agg_final_costs);
			get_agg_clause_costs(root, extra->havingQual,
								 AGGSPLIT_FINAL_DESERIAL,
								 agg_final_costs);
		}

		extra->partial_costs_set = true;
	}

	/* Estimate number of partial groups. */
	if (cheapest_total_path != NULL)
		dNumPartialGroups =
			get_number_of_groups(root,
								 cheapest_total_path->rows,
								 gd,
								 extra->targetList);
	if (cheapest_partial_path != NULL)
		dNumPartialPartialGroups =
			get_number_of_groups(root,
								 cheapest_partial_path->rows,
								 gd,
								 extra->targetList);

	if (can_sort && cheapest_total_path != NULL)
	{
		/* This should have been checked previously */
		Assert(parse->hasAggs || parse->groupClause);

		/*
		 * Use any available suitably-sorted path as input, and also consider
		 * sorting the cheapest partial path.
		 */
		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);
			bool		is_sorted;

			is_sorted = pathkeys_contained_in(root->group_pathkeys,
											  path->pathkeys);
			if (path == cheapest_total_path || is_sorted)
			{
				/* Sort the cheapest partial path, if it isn't already */
				if (!is_sorted)
					path = (Path *) create_sort_path(root,
													 partially_grouped_rel,
													 path,
													 root->group_pathkeys,
													 -1.0);

				if (parse->hasAggs)
					add_path(partially_grouped_rel, (Path *)
							 create_agg_path(root,
											 partially_grouped_rel,
											 path,
											 partially_grouped_rel->reltarget,
											 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
											 AGGSPLIT_INITIAL_SERIAL,
											 parse->groupClause,
											 NIL,
											 agg_partial_costs,
											 dNumPartialGroups));
				else
					add_path(partially_grouped_rel, (Path *)
							 create_group_path(root,
											   partially_grouped_rel,
											   path,
											   parse->groupClause,
											   NIL,
											   dNumPartialGroups));
			}
		}
	}

	if (can_sort && cheapest_partial_path != NULL)
	{
		/* Similar to above logic, but for partial paths. */
		foreach(lc, input_rel->partial_pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);
			bool		is_sorted;

			is_sorted = pathkeys_contained_in(root->group_pathkeys,
											  path->pathkeys);
			if (path == cheapest_partial_path || is_sorted)
			{
				/* Sort the cheapest partial path, if it isn't already */
				if (!is_sorted)
					path = (Path *) create_sort_path(root,
													 partially_grouped_rel,
													 path,
													 root->group_pathkeys,
													 -1.0);

				if (parse->hasAggs)
					add_partial_path(partially_grouped_rel, (Path *)
									 create_agg_path(root,
													 partially_grouped_rel,
													 path,
													 partially_grouped_rel->reltarget,
													 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
													 AGGSPLIT_INITIAL_SERIAL,
													 parse->groupClause,
													 NIL,
													 agg_partial_costs,
													 dNumPartialPartialGroups));
				else
					add_partial_path(partially_grouped_rel, (Path *)
									 create_group_path(root,
													   partially_grouped_rel,
													   path,
													   parse->groupClause,
													   NIL,
													   dNumPartialPartialGroups));
			}
		}
	}

	if (can_hash && cheapest_total_path != NULL)
	{
		Size		hashaggtablesize;

		/* Checked above */
		Assert(parse->hasAggs || parse->groupClause);

		hashaggtablesize =
			estimate_hashagg_tablesize(cheapest_total_path,
									   agg_partial_costs,
									   dNumPartialGroups);

		/*
		 * Tentatively produce a partial HashAgg Path, depending on if it
		 * looks as if the hash table will fit in work_mem.
		 */
		if (hashaggtablesize < work_mem * 1024L &&
			cheapest_total_path != NULL)
		{
			add_path(partially_grouped_rel, (Path *)
					 create_agg_path(root,
									 partially_grouped_rel,
									 cheapest_total_path,
									 partially_grouped_rel->reltarget,
									 AGG_HASHED,
									 AGGSPLIT_INITIAL_SERIAL,
									 parse->groupClause,
									 NIL,
									 agg_partial_costs,
									 dNumPartialGroups));
		}
	}

	if (can_hash && cheapest_partial_path != NULL)
	{
		Size		hashaggtablesize;

		hashaggtablesize =
			estimate_hashagg_tablesize(cheapest_partial_path,
									   agg_partial_costs,
									   dNumPartialPartialGroups);

		/* Do the same for partial paths. */
		if (hashaggtablesize < work_mem * 1024L &&
			cheapest_partial_path != NULL)
		{
			add_partial_path(partially_grouped_rel, (Path *)
							 create_agg_path(root,
											 partially_grouped_rel,
											 cheapest_partial_path,
											 partially_grouped_rel->reltarget,
											 AGG_HASHED,
											 AGGSPLIT_INITIAL_SERIAL,
											 parse->groupClause,
											 NIL,
											 agg_partial_costs,
											 dNumPartialPartialGroups));
		}
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding partially grouped ForeignPaths.
	 */
	if (partially_grouped_rel->fdwroutine &&
		partially_grouped_rel->fdwroutine->GetForeignUpperPaths)
	{
		FdwRoutine *fdwroutine = partially_grouped_rel->fdwroutine;

		fdwroutine->GetForeignUpperPaths(root,
										 UPPERREL_PARTIAL_GROUP_AGG,
										 input_rel, partially_grouped_rel,
										 extra);
	}

	return partially_grouped_rel;
}

/*
 * Generate Gather and Gather Merge paths for a grouping relation or partial
 * grouping relation.
 *
 * generate_gather_paths does most of the work, but we also consider a special
 * case: we could try sorting the data by the group_pathkeys and then applying
 * Gather Merge.
 *
 * NB: This function shouldn't be used for anything other than a grouped or
 * partially grouped relation not only because of the fact that it explicitly
 * references group_pathkeys but we pass "true" as the third argument to
 * generate_gather_paths().
 */
static void
gather_grouping_paths(PlannerInfo *root, RelOptInfo *rel)
{
	Path	   *cheapest_partial_path;

	/* Try Gather for unordered paths and Gather Merge for ordered ones. */
	generate_gather_paths(root, rel, true);

	/* Try cheapest partial path + explicit Sort + Gather Merge. */
	cheapest_partial_path = linitial(rel->partial_pathlist);
	if (!pathkeys_contained_in(root->group_pathkeys,
							   cheapest_partial_path->pathkeys))
	{
		Path	   *path;
		double		total_groups;

		total_groups =
			cheapest_partial_path->rows * cheapest_partial_path->parallel_workers;
		path = (Path *) create_sort_path(root, rel, cheapest_partial_path,
										 root->group_pathkeys,
										 -1.0);
		path = (Path *)
			create_gather_merge_path(root,
									 rel,
									 path,
									 rel->reltarget,
									 root->group_pathkeys,
									 NULL,
									 &total_groups);

		add_path(rel, path);
	}
}

/*
 * create_partitionwise_grouping_paths
 *
 * If the partition keys of input relation are part of the GROUP BY clause, all
 * the rows belonging to a given group come from a single partition.  This
 * allows aggregation/grouping over a partitioned relation to be broken down
 * into aggregation/grouping on each partition.  This should be no worse, and
 * often better, than the normal approach.
 *
 * However, if the GROUP BY clause does not contain all the partition keys,
 * rows from a given group may be spread across multiple partitions. In that
 * case, we perform partial aggregation for each group, append the results,
 * and then finalize aggregation.  This is less certain to win than the
 * previous case.  It may win if the PartialAggregate stage greatly reduces
 * the number of groups, because fewer rows will pass through the Append node.
 * It may lose if we have lots of small groups.
 */
static void
create_partitionwise_grouping_paths(PlannerInfo *root,
									RelOptInfo *input_rel,
									RelOptInfo *grouped_rel,
									RelOptInfo *partially_grouped_rel,
									const AggClauseCosts *agg_costs,
									grouping_sets_data *gd,
									PartitionwiseAggregateType patype,
									GroupPathExtraData *extra)
{
	int			nparts = input_rel->nparts;
	int			cnt_parts;
	List	   *grouped_live_children = NIL;
	List	   *partially_grouped_live_children = NIL;
	PathTarget *target = grouped_rel->reltarget;

	Assert(patype != PARTITIONWISE_AGGREGATE_NONE);
	Assert(patype != PARTITIONWISE_AGGREGATE_PARTIAL ||
		   partially_grouped_rel != NULL);

	/* Add paths for partitionwise aggregation/grouping. */
	for (cnt_parts = 0; cnt_parts < nparts; cnt_parts++)
	{
		RelOptInfo *child_input_rel = input_rel->part_rels[cnt_parts];
		PathTarget *child_target = copy_pathtarget(target);
		AppendRelInfo **appinfos;
		int			nappinfos;
		GroupPathExtraData child_extra;
		RelOptInfo *child_grouped_rel;
		RelOptInfo *child_partially_grouped_rel;

		/* Input child rel must have a path */
		Assert(child_input_rel->pathlist != NIL);

		/*
		 * Copy the given "extra" structure as is and then override the
		 * members specific to this child.
		 */
		memcpy(&child_extra, extra, sizeof(child_extra));

		appinfos = find_appinfos_by_relids(root, child_input_rel->relids,
										   &nappinfos);

		child_target->exprs = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) target->exprs,
								   nappinfos, appinfos);

		/* Translate havingQual and targetList. */
		child_extra.havingQual = (Node *)
			adjust_appendrel_attrs(root,
								   extra->havingQual,
								   nappinfos, appinfos);
		child_extra.targetList = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) extra->targetList,
								   nappinfos, appinfos);

		/*
		 * extra->patype was the value computed for our parent rel; patype is
		 * the value for this relation.  For the child, our value is its
		 * parent rel's value.
		 */
		child_extra.patype = patype;

		/*
		 * Create grouping relation to hold fully aggregated grouping and/or
		 * aggregation paths for the child.
		 */
		Assert(!child_input_rel->grouped_rel);
		child_grouped_rel = make_grouping_rel(root, child_input_rel,
											  child_target,
											  extra->target_parallel_safe,
											  child_extra.havingQual);
		child_input_rel->grouped_rel = child_grouped_rel;

		/* Ignore empty children. They contribute nothing. */
		if (IS_DUMMY_REL(child_input_rel))
		{
			mark_dummy_rel(child_grouped_rel);

			continue;
		}

		/* Create grouping paths for this child relation. */
		create_ordinary_grouping_paths(root, child_input_rel,
									   child_grouped_rel,
									   agg_costs, gd, &child_extra,
									   &child_partially_grouped_rel);

		if (child_partially_grouped_rel)
		{
			partially_grouped_live_children =
				lappend(partially_grouped_live_children,
						child_partially_grouped_rel);
		}

		if (patype == PARTITIONWISE_AGGREGATE_FULL)
		{
			set_cheapest(child_grouped_rel);
			grouped_live_children = lappend(grouped_live_children,
											child_grouped_rel);
		}

		pfree(appinfos);
	}

	/*
	 * All children can't be dummy at this point. If they are, then the parent
	 * too marked as dummy.
	 */
	Assert(grouped_live_children != NIL ||
		   partially_grouped_live_children != NIL);

	/*
	 * Try to create append paths for partially grouped children. For full
	 * partitionwise aggregation, we might have paths in the partial_pathlist
	 * if parallel aggregation is possible.  For partial partitionwise
	 * aggregation, we may have paths in both pathlist and partial_pathlist.
	 */
	if (partially_grouped_rel)
	{
		add_paths_to_append_rel(root, partially_grouped_rel,
								partially_grouped_live_children);

		/*
		 * We need call set_cheapest, since the finalization step will use the
		 * cheapest path from the rel.
		 */
		if (partially_grouped_rel->pathlist)
			set_cheapest(partially_grouped_rel);
	}

	/* If possible, create append paths for fully grouped children. */
	if (patype == PARTITIONWISE_AGGREGATE_FULL)
		add_paths_to_append_rel(root, grouped_rel, grouped_live_children);
}

/*
 * group_by_has_partkey
 *
 * Returns true, if all the partition keys of the given relation are part of
 * the GROUP BY clauses, false otherwise.
 */
static bool
group_by_has_partkey(RelOptInfo *input_rel,
					 List *targetList,
					 List *groupClause)
{
	List	   *groupexprs = get_sortgrouplist_exprs(groupClause, targetList);
	int			cnt = 0;
	int			partnatts;

	/* Input relation should be partitioned. */
	Assert(input_rel->part_scheme);

	/* Rule out early, if there are no partition keys present. */
	if (!input_rel->partexprs)
		return false;

	partnatts = input_rel->part_scheme->partnatts;

	for (cnt = 0; cnt < partnatts; cnt++)
	{
		List	   *partexprs = input_rel->partexprs[cnt];
		ListCell   *lc;
		bool		found = false;

		foreach(lc, partexprs)
		{
			Expr	   *partexpr = lfirst(lc);

			if (list_member(groupexprs, partexpr))
			{
				found = true;
				break;
			}
		}

		/*
		 * If none of the partition key expressions match with any of the
		 * GROUP BY expression, return false.
		 */
		if (!found)
			return false;
	}

	return true;
}


/*
 * For a given input path, consider the possible ways of doing grouping sets on
 * it, by combinations of hashing and sorting.  This can be called multiple
 * times, so it's important that it not scribble on input.  No result is
 * returned, but any generated paths are added to grouped_rel.
 */
static void
consider_groupingsets_paths(PlannerInfo *root,
							RelOptInfo *grouped_rel,
							Path *path,
							bool is_sorted,
							bool can_hash,
							grouping_sets_data *gd,
							const AggClauseCosts *agg_costs,
							double dNumGroups)
{
	Query	   *parse = root->parse;

	/*
	 * If we're not being offered sorted input, then only consider plans that
	 * can be done entirely by hashing.
	 *
	 * We can hash everything if it looks like it'll fit in work_mem. But if
	 * the input is actually sorted despite not being advertised as such, we
	 * prefer to make use of that in order to use less memory.
	 *
	 * If none of the grouping sets are sortable, then ignore the work_mem
	 * limit and generate a path anyway, since otherwise we'll just fail.
	 */
	if (!is_sorted)
	{
		List	   *new_rollups = NIL;
		RollupData *unhashed_rollup = NULL;
		List	   *sets_data;
		List	   *empty_sets_data = NIL;
		List	   *empty_sets = NIL;
		ListCell   *lc;
		ListCell   *l_start = list_head(gd->rollups);
		AggStrategy strat = AGG_HASHED;
		Size		hashsize;
		double		exclude_groups = 0.0;

		Assert(can_hash);

		/*
		 * If the input is coincidentally sorted usefully (which can happen
		 * even if is_sorted is false, since that only means that our caller
		 * has set up the sorting for us), then save some hashtable space by
		 * making use of that. But we need to watch out for degenerate cases:
		 *
		 * 1) If there are any empty grouping sets, then group_pathkeys might
		 * be NIL if all non-empty grouping sets are unsortable. In this case,
		 * there will be a rollup containing only empty groups, and the
		 * pathkeys_contained_in test is vacuously true; this is ok.
		 *
		 * XXX: the above relies on the fact that group_pathkeys is generated
		 * from the first rollup. If we add the ability to consider multiple
		 * sort orders for grouping input, this assumption might fail.
		 *
		 * 2) If there are no empty sets and only unsortable sets, then the
		 * rollups list will be empty (and thus l_start == NULL), and
		 * group_pathkeys will be NIL; we must ensure that the vacuously-true
		 * pathkeys_contain_in test doesn't cause us to crash.
		 */
		if (l_start != NULL &&
			pathkeys_contained_in(root->group_pathkeys, path->pathkeys))
		{
			unhashed_rollup = lfirst_node(RollupData, l_start);
			exclude_groups = unhashed_rollup->numGroups;
			l_start = lnext(l_start);
		}

		hashsize = estimate_hashagg_tablesize(path,
											  agg_costs,
											  dNumGroups - exclude_groups);

		/*
		 * gd->rollups is empty if we have only unsortable columns to work
		 * with.  Override work_mem in that case; otherwise, we'll rely on the
		 * sorted-input case to generate usable mixed paths.
		 */
		if (hashsize > work_mem * 1024L && gd->rollups)
			return;				/* nope, won't fit */

		/*
		 * We need to burst the existing rollups list into individual grouping
		 * sets and recompute a groupClause for each set.
		 */
		sets_data = list_copy(gd->unsortable_sets);

		for_each_cell(lc, l_start)
		{
			RollupData *rollup = lfirst_node(RollupData, lc);

			/*
			 * If we find an unhashable rollup that's not been skipped by the
			 * "actually sorted" check above, we can't cope; we'd need sorted
			 * input (with a different sort order) but we can't get that here.
			 * So bail out; we'll get a valid path from the is_sorted case
			 * instead.
			 *
			 * The mere presence of empty grouping sets doesn't make a rollup
			 * unhashable (see preprocess_grouping_sets), we handle those
			 * specially below.
			 */
			if (!rollup->hashable)
				return;
			else
				sets_data = list_concat(sets_data, list_copy(rollup->gsets_data));
		}
		foreach(lc, sets_data)
		{
			GroupingSetData *gs = lfirst_node(GroupingSetData, lc);
			List	   *gset = gs->set;
			RollupData *rollup;

			if (gset == NIL)
			{
				/* Empty grouping sets can't be hashed. */
				empty_sets_data = lappend(empty_sets_data, gs);
				empty_sets = lappend(empty_sets, NIL);
			}
			else
			{
				rollup = makeNode(RollupData);

				rollup->groupClause = preprocess_groupclause(root, gset);
				rollup->gsets_data = list_make1(gs);
				rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
														 rollup->gsets_data,
														 gd->tleref_to_colnum_map);
				rollup->numGroups = gs->numGroups;
				rollup->hashable = true;
				rollup->is_hashed = true;
				new_rollups = lappend(new_rollups, rollup);
			}
		}

		/*
		 * If we didn't find anything nonempty to hash, then bail.  We'll
		 * generate a path from the is_sorted case.
		 */
		if (new_rollups == NIL)
			return;

		/*
		 * If there were empty grouping sets they should have been in the
		 * first rollup.
		 */
		Assert(!unhashed_rollup || !empty_sets);

		if (unhashed_rollup)
		{
			new_rollups = lappend(new_rollups, unhashed_rollup);
			strat = AGG_MIXED;
		}
		else if (empty_sets)
		{
			RollupData *rollup = makeNode(RollupData);

			rollup->groupClause = NIL;
			rollup->gsets_data = empty_sets_data;
			rollup->gsets = empty_sets;
			rollup->numGroups = list_length(empty_sets);
			rollup->hashable = false;
			rollup->is_hashed = false;
			new_rollups = lappend(new_rollups, rollup);
			strat = AGG_MIXED;
		}

		add_path(grouped_rel, (Path *)
				 create_groupingsets_path(root,
										  grouped_rel,
										  path,
										  (List *) parse->havingQual,
										  strat,
										  new_rollups,
										  agg_costs,
										  dNumGroups));
		return;
	}

	/*
	 * If we have sorted input but nothing we can do with it, bail.
	 */
	if (list_length(gd->rollups) == 0)
		return;

	/*
	 * Given sorted input, we try and make two paths: one sorted and one mixed
	 * sort/hash. (We need to try both because hashagg might be disabled, or
	 * some columns might not be sortable.)
	 *
	 * can_hash is passed in as false if some obstacle elsewhere (such as
	 * ordered aggs) means that we shouldn't consider hashing at all.
	 */
	if (can_hash && gd->any_hashable)
	{
		List	   *rollups = NIL;
		List	   *hash_sets = list_copy(gd->unsortable_sets);
		double		availspace = (work_mem * 1024.0);
		ListCell   *lc;

		/*
		 * Account first for space needed for groups we can't sort at all.
		 */
		availspace -= (double) estimate_hashagg_tablesize(path,
														  agg_costs,
														  gd->dNumHashGroups);

		if (availspace > 0 && list_length(gd->rollups) > 1)
		{
			double		scale;
			int			num_rollups = list_length(gd->rollups);
			int			k_capacity;
			int		   *k_weights = palloc(num_rollups * sizeof(int));
			Bitmapset  *hash_items = NULL;
			int			i;

			/*
			 * We treat this as a knapsack problem: the knapsack capacity
			 * represents work_mem, the item weights are the estimated memory
			 * usage of the hashtables needed to implement a single rollup,
			 * and we really ought to use the cost saving as the item value;
			 * however, currently the costs assigned to sort nodes don't
			 * reflect the comparison costs well, and so we treat all items as
			 * of equal value (each rollup we hash instead saves us one sort).
			 *
			 * To use the discrete knapsack, we need to scale the values to a
			 * reasonably small bounded range.  We choose to allow a 5% error
			 * margin; we have no more than 4096 rollups in the worst possible
			 * case, which with a 5% error margin will require a bit over 42MB
			 * of workspace. (Anyone wanting to plan queries that complex had
			 * better have the memory for it.  In more reasonable cases, with
			 * no more than a couple of dozen rollups, the memory usage will
			 * be negligible.)
			 *
			 * k_capacity is naturally bounded, but we clamp the values for
			 * scale and weight (below) to avoid overflows or underflows (or
			 * uselessly trying to use a scale factor less than 1 byte).
			 */
			scale = Max(availspace / (20.0 * num_rollups), 1.0);
			k_capacity = (int) floor(availspace / scale);

			/*
			 * We leave the first rollup out of consideration since it's the
			 * one that matches the input sort order.  We assign indexes "i"
			 * to only those entries considered for hashing; the second loop,
			 * below, must use the same condition.
			 */
			i = 0;
			for_each_cell(lc, lnext(list_head(gd->rollups)))
			{
				RollupData *rollup = lfirst_node(RollupData, lc);

				if (rollup->hashable)
				{
					double		sz = estimate_hashagg_tablesize(path,
																agg_costs,
																rollup->numGroups);

					/*
					 * If sz is enormous, but work_mem (and hence scale) is
					 * small, avoid integer overflow here.
					 */
					k_weights[i] = (int) Min(floor(sz / scale),
											 k_capacity + 1.0);
					++i;
				}
			}

			/*
			 * Apply knapsack algorithm; compute the set of items which
			 * maximizes the value stored (in this case the number of sorts
			 * saved) while keeping the total size (approximately) within
			 * capacity.
			 */
			if (i > 0)
				hash_items = DiscreteKnapsack(k_capacity, i, k_weights, NULL);

			if (!bms_is_empty(hash_items))
			{
				rollups = list_make1(linitial(gd->rollups));

				i = 0;
				for_each_cell(lc, lnext(list_head(gd->rollups)))
				{
					RollupData *rollup = lfirst_node(RollupData, lc);

					if (rollup->hashable)
					{
						if (bms_is_member(i, hash_items))
							hash_sets = list_concat(hash_sets,
													list_copy(rollup->gsets_data));
						else
							rollups = lappend(rollups, rollup);
						++i;
					}
					else
						rollups = lappend(rollups, rollup);
				}
			}
		}

		if (!rollups && hash_sets)
			rollups = list_copy(gd->rollups);

		foreach(lc, hash_sets)
		{
			GroupingSetData *gs = lfirst_node(GroupingSetData, lc);
			RollupData *rollup = makeNode(RollupData);

			Assert(gs->set != NIL);

			rollup->groupClause = preprocess_groupclause(root, gs->set);
			rollup->gsets_data = list_make1(gs);
			rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
													 rollup->gsets_data,
													 gd->tleref_to_colnum_map);
			rollup->numGroups = gs->numGroups;
			rollup->hashable = true;
			rollup->is_hashed = true;
			rollups = lcons(rollup, rollups);
		}

		if (rollups)
		{
			add_path(grouped_rel, (Path *)
					 create_groupingsets_path(root,
											  grouped_rel,
											  path,
											  (List *) parse->havingQual,
											  AGG_MIXED,
											  rollups,
											  agg_costs,
											  dNumGroups));
		}
	}

	/*
	 * Now try the simple sorted case.
	 */
	if (!gd->unsortable_sets)
		add_path(grouped_rel, (Path *)
				 create_groupingsets_path(root,
										  grouped_rel,
										  path,
										  (List *) parse->havingQual,
										  AGG_SORTED,
										  gd->rollups,
										  agg_costs,
										  dNumGroups));
}


/*
 * Given a groupclause and a list of GroupingSetData, return equivalent sets
 * (without annotation) mapped to indexes into the given groupclause.
 */
List *
remap_to_groupclause_idx(List *groupClause,
						 List *gsets,
						 int *tleref_to_colnum_map)
{
	int			ref = 0;
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, groupClause)
	{
		SortGroupClause *gc = lfirst_node(SortGroupClause, lc);

		tleref_to_colnum_map[gc->tleSortGroupRef] = ref++;
	}

	foreach(lc, gsets)
	{
		List	   *set = NIL;
		ListCell   *lc2;
		GroupingSetData *gs = lfirst_node(GroupingSetData, lc);

		foreach(lc2, gs->set)
		{
			set = lappend_int(set, tleref_to_colnum_map[lfirst_int(lc2)]);
		}

		result = lappend(result, set);
	}

	return result;
}

/*
 * make_group_input_target
 *	  Generate appropriate PathTarget for initial input to grouping nodes.
 *
 * If there is grouping or aggregation, the scan/join subplan cannot emit
 * the query's final targetlist; for example, it certainly can't emit any
 * aggregate function calls.  This routine generates the correct target
 * for the scan/join subplan.
 *
 * The query target list passed from the parser already contains entries
 * for all ORDER BY and GROUP BY expressions, but it will not have entries
 * for variables used only in HAVING clauses; so we need to add those
 * variables to the subplan target list.  Also, we flatten all expressions
 * except GROUP BY items into their component variables; other expressions
 * will be computed by the upper plan nodes rather than by the subplan.
 * For example, given a query like
 *		SELECT a+b,SUM(c+d) FROM table GROUP BY a+b;
 * we want to pass this targetlist to the subplan:
 *		a+b,c,d
 * where the a+b target will be used by the Sort/Group steps, and the
 * other targets will be used for computing the final results.
 *
 * 'final_target' is the query's final target list (in PathTarget form)
 *
 * The result is the PathTarget to be computed by the Paths returned from
 * query_planner().
 */
/*
 * Adds anything needed to compute HAVING, as well as GROUP BY columns,
 * to the target list. 'rel' is the relation at which we're evaluating
 * the grouping.
 */
PathTarget *
make_group_input_target(PlannerInfo *root, RelOptInfo *rel)
{
	Query	   *parse = root->parse;
	PathTarget *final_target = root->final_target;
	PathTarget *input_target;
	List	   *non_group_cols;
	List	   *non_group_vars;
	int			i;
	ListCell   *lc;
	ListCell *lec;
	ListCell *lsortref;

	/*
	 * We must build a target containing all grouping columns, plus any other
	 * Vars mentioned in the query's targetlist and HAVING qual.
	 */
	input_target = create_empty_pathtarget();
	non_group_cols = NIL;

	/*
	 * Add any grouping columns.
	 *
	 * This matters, when we decide to compute the grouping based on a different
	 * column than is listed in groupClause, which we know to be equivalent.
	 */
	forboth(lec, root->group_ecs, lsortref, root->group_sortrefs)
	{
		EquivalenceClass *eclass = lfirst_node(EquivalenceClass, lec);
		int			sgref = lfirst_int(lsortref);
		Expr	   *expr;

		if (eclass)
		{
			expr = find_em_expr_for_rel(eclass, rel);
			if (!expr)
				elog(ERROR, "could not find equivalence class member for given relations");
		}
		else
		{
			expr = get_sortgroupref_tle(sgref, root->processed_tlist)->expr;
		}

		add_column_to_pathtarget(input_target, expr, sgref);
	}

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		if (bms_is_subset(pull_varnos((Node *) expr), rel->relids))
		{
			/*
			 * Non-grouping column, so just remember the expression for later
			 * call to pull_var_clause.
			 */
			non_group_cols = lappend(non_group_cols, expr);
		}

		i++;
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars it uses, too.
	 */
	if (parse->havingQual)
		non_group_cols = lappend(non_group_cols, parse->havingQual);

	/*
	 * Pull out all the Vars mentioned in non-group cols (plus HAVING), and
	 * add them to the input target if not already present.  (A Var used
	 * directly as a GROUP BY item will be present already.)  Note this
	 * includes Vars used in resjunk items, so we are covering the needs of
	 * ORDER BY and window specifications.  Vars used within Aggrefs and
	 * WindowFuncs will be pulled out here, too.
	 */
	non_group_vars = pull_var_clause((Node *) non_group_cols,
									 PVC_RECURSE_AGGREGATES |
									 PVC_RECURSE_WINDOWFUNCS |
									 PVC_INCLUDE_PLACEHOLDERS);

	add_new_columns_to_pathtarget(input_target, non_group_vars);

	/* XXX: Add anything needed to evaluate Aggs here, i.e. agg arguments */

	/* clean up cruft */
	list_free(non_group_vars);
	list_free(non_group_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, input_target);
}

PathTarget *
make_grouping_target(PlannerInfo *root, RelOptInfo *rel, PathTarget *input_target, PathTarget *final_target)
{
	/*
	 * - grouping columns
	 * - aggregates
	 * - columns needed for joins above this node
	 */
	Query	   *parse = root->parse;
	PathTarget *grouping_target;
	List	   *non_group_cols;
	List	   *non_group_vars;
	int			i;
	ListCell   *lc;
	Relids		relids;
	Bitmapset  *group_col_sortrefs = NULL;

	/*
	 * We must build a target containing all grouping columns, plus any other
	 * Vars mentioned in the query's targetlist. We can ignore HAVING here,
	 * it's been evaluated at the Grouping node already.
	 */
	grouping_target = create_empty_pathtarget();
	non_group_cols = NIL;

	/*
	 * 1. Take the input target list. It should include all grouping cols. Remove everything that's
	 * not a grouping col.
	 * 2. Add Aggrefs.
	 */

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(final_target, i);

		if (bms_is_subset(pull_varnos((Node *) expr), rel->relids))
		{
			if (sgref && parse->groupClause &&
				get_sortgroupref_clause_noerr(sgref, parse->groupClause) != NULL)
			{
				/*
				 * It's a grouping column, so add it to the input target as-is.
				 */
				group_col_sortrefs = bms_add_member(group_col_sortrefs, sgref);
				add_column_to_pathtarget(grouping_target, expr, sgref);
			}
			else
			{
				/*
				 * Non-grouping column, so just remember the expression for later
				 * call to pull_var_clause.
				 */
				non_group_cols = lappend(non_group_cols, expr);
			}
		}

		i++;
	}

	/* attrs_needed refers to parent relids and not those of a child. */
	if (rel->top_parent_relids)
		relids = rel->top_parent_relids;
	else
		relids = rel->relids;

	relids = bms_add_member(bms_copy(relids), NEEDED_IN_GROUPING);

	i = 0;
	foreach(lc, input_target->exprs)
	{
		Var		   *var = (Var *) lfirst(lc);
		RelOptInfo *baserel;
		int			ndx;
		Index		sgref = get_pathtarget_sortgroupref(input_target, i);

		/* this is similar to build_joinrel_tlist. */

		if (sgref && bms_is_member(sgref, group_col_sortrefs))
		{
			i++;
			continue;
		}

		/*
		 * Ignore PlaceHolderVars in the input tlists; we'll make our own
		 * decisions about whether to copy them.
		 */
		if (IsA(var, PlaceHolderVar))
		{
			i++;
			continue;
		}

		/*
		 * Otherwise, anything in a baserel or joinrel targetlist ought to be
		 * a Var. Children of a partitioned table may have ConvertRowtypeExpr
		 * translating whole-row Var of a child to that of the parent.
		 * Children of an inherited table or subquery child rels can not
		 * directly participate in a join, so other kinds of nodes here.
		 */
		if (IsA(var, Var))
		{
			baserel = find_base_rel(root, var->varno);
			ndx = var->varattno - baserel->min_attr;
		}
		else if (IsA(var, ConvertRowtypeExpr))
		{
			ConvertRowtypeExpr *child_expr = (ConvertRowtypeExpr *) var;
			Var		   *childvar = (Var *) child_expr->arg;

			/*
			 * Child's whole-row references are converted to look like those
			 * of parent using ConvertRowtypeExpr. There can be as many
			 * ConvertRowtypeExpr decorations as the depth of partition tree.
			 * The argument to the deepest ConvertRowtypeExpr is expected to
			 * be a whole-row reference of the child.
			 */
			while (IsA(childvar, ConvertRowtypeExpr))
			{
				child_expr = (ConvertRowtypeExpr *) childvar;
				childvar = (Var *) child_expr->arg;
			}
			Assert(IsA(childvar, Var) &&childvar->varattno == 0);

			baserel = find_base_rel(root, childvar->varno);
			ndx = 0 - baserel->min_attr;
		}
		else
		{
			/*
			 * If this rel is above grouping, then we can have Aggrefs
			 * and grouping column expressions in the target list. Carry
			 * them up to the join rel. They will surely be needed at
			 * the top of the join tree. (Unless they're only used in
			 * HAVING?)
			 */
#if 0
			elog(ERROR, "unexpected node type in rel targetlist: %d",
				 (int) nodeTag(var));
#endif
			baserel = NULL;
		}

		/* Is the target expression still needed above this joinrel? */
		if (baserel == NULL || bms_nonempty_difference(baserel->attr_needed[ndx], relids))
		{
			/* Yup, add it to the output */
			add_column_to_pathtarget(grouping_target, (Expr *) var, sgref);
		}
		i++;
	}

	/*
	 * Pull out all the Vars mentioned in non-group cols, and
	 * add them to the input target if not already present.  (A Var used
	 * directly as a GROUP BY item will be present already.)  Note this
	 * includes Vars used in resjunk items, so we are covering the needs of
	 * ORDER BY and window specifications.  Vars used within
	 * WindowFuncs will be pulled out here, too. Aggrefs will be included
	 * as is.
	 */
	non_group_vars = pull_var_clause((Node *) non_group_cols,
									 PVC_INCLUDE_AGGREGATES |
									 PVC_RECURSE_WINDOWFUNCS |
									 PVC_INCLUDE_PLACEHOLDERS);
	foreach (lc, non_group_vars)
	{
		Node *n = lfirst(lc);

		if (IsA(n, Aggref))
			add_new_column_to_pathtarget(grouping_target, (Expr *) n);
	}

	add_new_columns_to_pathtarget(grouping_target, non_group_vars);

	/* XXX: Add anything needed to evaluate Aggs here, i.e. agg arguments */

	/* clean up cruft */
	list_free(non_group_vars);
	list_free(non_group_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, grouping_target);
}

/*
 * Is the GROUP BY computable based on the given 'relids'?
 *
 * From "Including Group-By in Query Optimization" paper:
 *
 * Definition 3.1: A node n of a given left-deep tree has the invariant
 * grouping property if the following conditions are true:
 *
 * 1. Every aggregating column of the query is a candidate aggregating
 * column of n.
 *
 * 2. Every join column of n is also a grouping column of the query.
 *
 * 3. For every join-node that is an ancestor of n, the join is an
 * equijoin predicate on a foreign key column of n.
 */
bool
is_grouping_computable_at_rel(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;
	ListCell   *lec;
	Relids		relids;
	Relids		other_relids;
	int			x;

	if (bms_is_subset(root->all_baserels, rel->relids))
	{
		/*
		 * If this is the final, top, join node, then surely the grouping
		 * can be done here.
		 */
		return true;
	}

	/*
	 * Currently, give up on SRFs in target list. It gets too complicated to
	 * evaluate them in the middle of the join tree. (Note that we check for
	 * this after checking if this is the final rel, so we still produce
	 * grouping plans with SRFs, at the top)
	 */
	if (root->parse->hasTargetSRFs)
		return false;

	/*
	 * 1. Every aggregating column of the query is a candidate aggregating
	 * column of n.
	 *
	 * What this means is that we must be able to compute the aggregates
	 * at this relation. For example, "AVG(tbl.col)" can only be computed
	 * if 'tbl' is part of this join relation.
	 */
	if (!bms_is_subset(root->agg_relids, rel->relids))
		return false;

	relids = rel->relids;

	/*
	 * We must also be able to compute each grouping column here.
	 */
	foreach (lec, root->group_ecs)
	{
		EquivalenceClass *ec = lfirst_node(EquivalenceClass, lec);

		if (!find_em_expr_for_rel(ec, rel))
			return false;
	}

	/*
	 * non-equijoins can only be evaluated correctly before grouping.
	 *
	 * XXX: A parameterized path, for use in the inner side of a nested
	 * loop join, where all the vars are available as Params, would be
	 * acceptable, though.
	 */
	foreach (lc, rel->joininfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (!bms_is_subset(rinfo->required_relids, rel->relids))
			return false;
	}

	x = -1;
	other_relids = bms_difference(root->all_baserels, relids);
	while ((x = bms_next_member(other_relids, x)) >= 0)
	{
		List	   *joinquals;
		Relids		joinrelids;
		Relids		outer_relids;
		RelOptInfo *other_rel;

		other_rel = find_base_rel(root, x);

		outer_relids = bms_make_singleton(x);
		joinrelids = bms_add_members(bms_make_singleton(x), relids);

		joinquals = generate_join_implied_equalities(root,
													 joinrelids,
													 outer_relids,
													 other_rel);

		/*
		 * Check condition 2: the join column must be in GROUP BY.
		 */
		foreach(lc, joinquals)
		{
			RestrictInfo *joinqual = lfirst_node(RestrictInfo, lc);

			if (!joinqual->can_join)
			{
				/* Not a joinable binary opclause */
				return false;
			}

			foreach (lec, root->group_ecs)
			{
				EquivalenceClass *ec = lfirst_node(EquivalenceClass, lec);

				/* XXX: are left_ec/right_ec guaranteed to be valid here? */
				if (ec == joinqual->left_ec ||
					ec == joinqual->right_ec)
				{
					break;
				}
			}
			if (lec == NULL)
			{
				/* This join qual was not in GROUP BY */
				return false;
			}
		}

		/*
		 * Check condition 3: the join mustn't "add" any more rows
		 */
		if (!innerrel_is_unique(root,
								joinrelids, /* joinrelids */
								relids, /* outerrelids */
								other_rel, /* innerrel */
								JOIN_INNER, /* XXX */
								joinquals,
								false)) /* force_cache */
		{
			return false;
		}
	}

	return true;
}
