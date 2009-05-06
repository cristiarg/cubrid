/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * query_graph.c - builds a query graph from a parse tree and
 * 			transforms the tree by unfolding path expressions.
 */

#ident "$Id$"

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#if !defined(WINDOWS)
#include <values.h>
#endif /* !WINDOWS */

#include "error_code.h"
#include "error_manager.h"
#include "object_representation.h"
#include "optimizer.h"
#include "query_graph.h"
#include "query_planner.h"
#include "schema_manager.h"
#include "statistics.h"
#include "system_parameter.h"
#include "parser.h"
#include "environment_variable.h"
#include "xasl_generation.h"
#include "parser.h"
#include "query_list.h"
#include "db.h"
#include "system_parameter.h"
#include "memory_alloc.h"
#include "environment_variable.h"
#include "util_func.h"
#include "parser.h"

#include "parser.h"
#include "locator_cl.h"
#include "object_domain.h"

/* figure out how many bytes a QO_USING_INDEX struct with n entries requires */
#define SIZEOF_USING_INDEX(n) \
    (sizeof(QO_USING_INDEX) + (((n)-1) * sizeof(QO_USING_INDEX_ENTRY)))

#define ALLOC_USING_INDEX(env, n) \
    (QO_USING_INDEX *)malloc(SIZEOF_USING_INDEX(n))

/* any number that won't overlap PT_MISC_TYPE */
#define PREDICATE_TERM  -2


/* used by build_graph_for_entity() */
#define IS_OID(name_spec) \
    (((name_spec)->node_type == PT_NAME) \
     && PT_IS_OID_NAME(name_spec))

#define RANK_DEFAULT       0	/* default              */
#define RANK_NAME          RANK_DEFAULT	/* name  -- use default */
#define RANK_VALUE         RANK_DEFAULT	/* value -- use default */
#define RANK_EXPR_LIGHT    1	/* Group 1              */
#define RANK_EXPR_MEDIUM   2	/* Group 2              */
#define RANK_EXPR_HEAVY    3	/* Group 3              */
#define RANK_EXPR_FUNCTION 4	/* agg function, set    */
#define RANK_QUERY         8	/* subquery             */

/*
 * Figure out how many bytes a QO_INDEX struct with n entries requires.
 */
#define SIZEOF_INDEX(n) \
    (sizeof(QO_INDEX) + (((n)-1)* sizeof(QO_INDEX_ENTRY)))

/*
 * Malloc a QO_INDEX struct with n entries.
 */
#define ALLOC_INDEX(env, n) \
    (QO_INDEX *)malloc(SIZEOF_INDEX(n))

/*
 * Figure out how many bytes a QO_CLASS_INFO struct with n entries requires.
 */
#define SIZEOF_CLASS_INFO(n) \
    (sizeof(QO_CLASS_INFO) + (((n)-1) * sizeof(QO_CLASS_INFO_ENTRY)))

/*
 * Malloc a QO_CLASS_INFO struct with n entries.
 */
#define ALLOC_CLASS_INFO(env, n) \
    (QO_CLASS_INFO *)malloc(SIZEOF_CLASS_INFO(n))

/*
 * Malloc a QO_ATTR_INFO struct with n entries.
 */
#define ALLOC_ATTR_INFO(env) \
    (QO_ATTR_INFO *)malloc(sizeof(QO_ATTR_INFO))

/*
 * Figure out how many bytes a pkeys[] struct with n entries requires.
 */
#define SIZEOF_ATTR_CUM_STATS_PKEYS(n) \
    ((n) * sizeof(int))

/*
 * Malloc a pkeys[] struct with n entries.
 */
#define ALLOC_ATTR_CUM_STATS_PKEYS(env, n) \
    (int *)malloc(SIZEOF_ATTR_CUM_STATS_PKEYS(n))

#define NOMINAL_HEAP_SIZE(class)	200	/* pages */
#define NOMINAL_OBJECT_SIZE(class)	 64	/* bytes */

/* Figure out how many bytes a QO_NODE_INDEX struct with n entries requires. */
#define SIZEOF_NODE_INDEX(n) \
    (sizeof(QO_NODE_INDEX) + (((n)-1)* sizeof(QO_NODE_INDEX_ENTRY)))

#define EXCHANGE_BUILDER(type,e0,e1) \
    do { type _tmp = e0; e0 = e1; e1 = _tmp; } while (0)

#define TERMCLASS_EXCHANGE(e0,e1)  EXCHANGE_BUILDER(QO_TERMCLASS,e0,e1)
#define DOUBLE_EXCHANGE(e0,e1)     EXCHANGE_BUILDER(double,e0,e1)
#define PT_NODE_EXCHANGE(e0,e1)    EXCHANGE_BUILDER(PT_NODE *,e0,e1)
#define INT_EXCHANGE(e0,e1)        EXCHANGE_BUILDER(int,e0,e1)
#define SEGMENTPTR_EXCHANGE(e0,e1) EXCHANGE_BUILDER(QO_SEGMENT *,e0,e1)
#define NODEPTR_EXCHANGE(e0,e1)    EXCHANGE_BUILDER(QO_NODE *,e0,e1)
#define EQCLASSPTR_EXCHANGE(e0,e1) EXCHANGE_BUILDER(QO_EQCLASS *,e0,e1)
#define BOOL_EXCHANGE(e0,e1)       EXCHANGE_BUILDER(bool,e0,e1)
#define JOIN_TYPE_EXCHANGE(e0,e1)  EXCHANGE_BUILDER(JOIN_TYPE,e0,e1)
#define FLAG_EXCHANGE(e0,e1)       EXCHANGE_BUILDER(int,e0,e1)

#define BISET_EXCHANGE(s0,s1) \
    do { \
	BITSET tmp; \
	BITSET_MOVE(tmp, s0); \
	BITSET_MOVE(s0, s1); \
	BITSET_MOVE(s1, tmp); \
    } while (0)

#define PUT_FLAG(cond, flag) \
    do { \
	if (cond) { \
	    if (extra_info++) { \
	        fputs(flag, f); \
	    } else { \
		fputs(" (", f); \
		fputs(flag, f); \
	    } \
	} \
    } while (0)
typedef enum
{
  QO_BUILD_ENTITY = 0x01,	/* 0000 0001 */
  QO_BUILD_PATH = 0x02		/* 0000 0010 */
} QO_BUILD_STATUS;

typedef struct walk_info WALK_INFO;
struct walk_info
{
  QO_ENV *env;
  QO_TERM *term;
};

double QO_INFINITY = 0.0;

static QO_PLAN *qo_optimize_helper (QO_ENV * env);

static QO_NODE *qo_add_node (PT_NODE * entity, QO_ENV * env);

static QO_SEGMENT *qo_insert_segment (QO_NODE * head, QO_NODE * tail,
				      PT_NODE * node, QO_ENV * env);

static QO_SEGMENT *qo_join_segment (QO_NODE * head, QO_NODE * tail,
				    PT_NODE * name, QO_ENV * env);

static PT_NODE *qo_add_final_segment (PARSER_CONTEXT * parser, PT_NODE * tree,
				      void *arg, int *continue_walk);

static QO_TERM *qo_add_term (PT_NODE * conjunct, int term_type, QO_ENV * env);

static void
qo_add_dep_term (QO_NODE * derived_node, BITSET * depend_nodes,
		 BITSET * depend_segs, QO_ENV * env);

static QO_TERM *qo_add_dummy_join_term (QO_ENV * env, QO_NODE * head_node,
					QO_NODE * tail_node);

static void qo_analyze_term (QO_TERM * term, int term_type);

static PT_NODE *set_seg_expr (PARSER_CONTEXT * parser, PT_NODE * tree,
			      void *arg, int *continue_walk);

static void set_seg_node (PT_NODE * attr, QO_ENV * env, BITSET * bitset);

static QO_ENV *qo_env_init (PARSER_CONTEXT * parser, PT_NODE * query);

static bool qo_validate (QO_ENV * env);

static PT_NODE *build_query_graph (PARSER_CONTEXT * parser, PT_NODE * tree,
				   void *arg, int *continue_walk);

static PT_NODE *build_query_graph_post (PARSER_CONTEXT * parser,
					PT_NODE * tree, void *arg,
					int *continue_walk);

static QO_NODE *build_graph_for_entity (QO_ENV * env, PT_NODE * entity,
					QO_BUILD_STATUS status);

static PT_NODE *graph_size_select (PARSER_CONTEXT * parser, PT_NODE * tree,
				   void *arg, int *continue_walk);

static void graph_size_for_entity (QO_ENV * env, PT_NODE * entity);

static bool is_dependent_table (PT_NODE * entity);

static void get_term_subqueries (QO_ENV * env, QO_TERM * term);

static void get_term_rank (QO_ENV * env, QO_TERM * term);

static PT_NODE *check_subquery_pre (PARSER_CONTEXT * parser, PT_NODE * node,
				    void *arg, int *continue_walk);

static bool is_local_name (QO_ENV * env, PT_NODE * expr);

static bool is_pseudo_const (QO_ENV * env, PT_NODE * expr);

static void get_local_subqueries (QO_ENV * env, PT_NODE * tree);

static void get_rank (QO_ENV * env);

static PT_NODE *get_referenced_attrs (PT_NODE * entity);

static bool expr_is_mergable (PT_NODE * pt_expr);

static bool expr_is_equi_join (PT_NODE * pt_expr);

static void add_hint (QO_ENV * env, PT_NODE * tree);

static void add_using_index (QO_ENV * env, PT_NODE * using_index);

static int get_opcode_rank (PT_OP_TYPE opcode);

static int get_operand_rank (PT_NODE * node);

static int count_classes (PT_NODE * p);
static QO_CLASS_INFO_ENTRY *grok_classes (QO_ENV * env, PT_NODE * dom_set,
					  QO_CLASS_INFO_ENTRY * info);
static int qo_data_compare (DB_DATA * data1, DB_DATA * data2, DB_TYPE type);
static void qo_estimate_statistics (MOP class_mop, CLASS_STATS *);

static void qo_node_free (QO_NODE *);
static void qo_node_dump (QO_NODE *, FILE *);
static void qo_node_add_sarg (QO_NODE *, QO_TERM *);

static void qo_seg_free (QO_SEGMENT *);

static QO_EQCLASS *qo_eqclass_new (QO_ENV *);
static void qo_eqclass_free (QO_EQCLASS *);
static void qo_eqclass_add (QO_EQCLASS *, QO_SEGMENT *);
static void qo_eqclass_dump (QO_EQCLASS *, FILE *);

static void qo_term_free (QO_TERM *);
static void qo_term_dump (QO_TERM *, FILE *);
static void qo_subquery_dump (QO_ENV *, QO_SUBQUERY *, FILE *);
static void qo_subquery_free (QO_SUBQUERY *);

static void qo_partition_init (QO_ENV *, QO_PARTITION *, int);
static void qo_partition_free (QO_PARTITION *);
static void qo_partition_dump (QO_PARTITION *, FILE *);
static void qo_find_index_terms (QO_ENV * env, BITSET * segsp,
				 BITSET * termsp);
static void qo_find_index_seg_terms (QO_ENV * env, int seg_idx,
				     BITSET * seg_equal_termsp,
				     BITSET * seg_other_termsp);
static bool qo_find_index_segs (QO_ENV *,
				SM_CLASS_CONSTRAINT *, QO_NODE *,
				int *, int, int *, BITSET *);
static void qo_find_node_indexes (QO_ENV *, QO_NODE *);
static int is_equivalent_indexes (QO_INDEX_ENTRY * index1,
				  QO_INDEX_ENTRY * index2);
static int qo_find_matching_index (QO_INDEX_ENTRY * index_entry,
				   QO_INDEX * class_indexes);
static QO_INDEX_ENTRY *is_index_compatible (QO_CLASS_INFO * class_info,
					    int n,
					    QO_INDEX_ENTRY * index_entry);


static void qo_equivalence (QO_SEGMENT *, QO_SEGMENT *);
static void qo_seg_nodes (QO_ENV *, BITSET *, BITSET *);
static QO_ENV *qo_env_new (PARSER_CONTEXT *, PT_NODE *);
static void qo_discover_partitions (QO_ENV *);
static void qo_discover_indexes (QO_ENV *);
static void qo_assign_eq_classes (QO_ENV *);
static void qo_discover_edges (QO_ENV *);
static void qo_term_clear (QO_ENV *, int);
static void qo_seg_clear (QO_ENV *, int);
static void qo_node_clear (QO_ENV *, int);
static void qo_get_index_info (QO_ENV * env, QO_NODE * node);
static void qo_free_index (QO_ENV * env, QO_INDEX *);
static QO_INDEX *qo_alloc_index (QO_ENV * env, int);
static void qo_free_node_index_info (QO_ENV * env,
				     QO_NODE_INDEX * node_indexp);
static void qo_free_attr_info (QO_ENV * env, QO_ATTR_INFO * info);
static QO_ATTR_INFO *qo_get_attr_info (QO_ENV * env, QO_SEGMENT * seg);
static void qo_free_class_info (QO_ENV * env, QO_CLASS_INFO *);
static QO_CLASS_INFO *qo_get_class_info (QO_ENV * env, QO_NODE * node);
static QO_SEGMENT *qo_eqclass_wrt (QO_EQCLASS *, BITSET *);
static void qo_env_dump (QO_ENV *, FILE *);



/*
 * qo_get_optimization_param () - Return the current value of some (global)
 *				  optimization parameter
 *   return: int
 *   retval(in): pointer to area to receive info
 *   param(in): what parameter to retrieve
 *   ...(in): parameter-specific parameters
 */
void
qo_get_optimization_param (void *retval, QO_PARAM param, ...)
{
  char *buf;
  va_list args;

  va_start (args, param);

  switch (param)
    {
    case QO_PARAM_LEVEL:
      *(int *) retval = PRM_OPTIMIZATION_LEVEL;
      break;
    case QO_PARAM_COST:
      buf = (char *) retval;
      buf[0] = (char) qo_plan_get_cost_fn (va_arg (args, char *));
      buf[1] = '\0';
      break;
    }

  va_end (args);
}

/*
 * qo_set_optimization_param () - Return the old value of some (global)
 *				  optimization param, and set the global
 *				  param to the new value
 *   return: int
 *   retval(in): pointer to area to receive info about old value
 *   param(in): what parameter to retrieve
 *   ...(in): parameter-specific parameters
 */
void
qo_set_optimization_param (void *retval, QO_PARAM param, ...)
{
  va_list args;
  va_start (args, param);

  switch (param)
    {
    case QO_PARAM_LEVEL:
      if (retval)
	*(int *) retval = PRM_OPTIMIZATION_LEVEL;
      PRM_OPTIMIZATION_LEVEL = va_arg (args, int);
      break;

    case QO_PARAM_COST:
      {
	const char *plan_name;
	int cost_fn;

	plan_name = va_arg (args, char *);
	cost_fn = va_arg (args, int);
	plan_name = qo_plan_set_cost_fn (plan_name, cost_fn);
	if (retval)
	  *(const char **) retval = plan_name;
	break;
      }
    }

  va_end (args);
}

/*
 * qo_optimize_query () - optimize a single select statement, skip nested
 *			  selects since embedded selects are optimized first
 *   return: void
 *   parser(in): parser environment
 *   tree(in): select tree to optimize
 */
QO_PLAN *
qo_optimize_query (PARSER_CONTEXT * parser, PT_NODE * tree)
{
  QO_ENV *env;
  int level;

  /*
   * Give up right away if the optimizer has been turned off in the
   * user's cubrid.conf file or somewhere else.
   */
  qo_get_optimization_param (&level, QO_PARAM_LEVEL);
  if (!OPTIMIZATION_ENABLED (level))
    {
      return NULL;
    }

  /* if its not a select, we're not interested, also if it is
   * merge we give up.
   */
  if (tree->node_type != PT_SELECT)
    {
      return NULL;
    }

  env = qo_env_init (parser, tree);
  if (env == NULL)
    {
      /* we can't optimize, so bail out */
      return NULL;
    }

  switch (setjmp (env->catch_))
    {
    case 0:
      /*
       * The return here is ok; we'll take care of freeing the env
       * structure later, when qo_plan_discard is called.  In fact, if
       * we free it now, the plan pointer we're about to return will be
       * worthless.
       */
      return qo_optimize_helper (env);
    case 1:
      /*
       * Out of memory during optimization.  malloc() has already done
       * an er_set().
       */
      break;
    case 2:
      /*
       * Failed some optimizer assertion.  QO_ABORT() has already done
       * an er_set().
       */
      break;
    default:
      /*
       * No clue.
       */
      er_set (ER_ERROR_SEVERITY, __FILE__, __LINE__,
	      ER_QO_FAILED_ASSERTION, 0);
      break;
    }

  /*
   * If we get here, an error of some sort occurred, and we need to
   * tear down everything and get out.
   */
#if defined(CUBRID_DEBUG)
  fprintf (stderr, "*** optimizer aborting ***\n");
#endif /* CUBRID_DEBUG */
  qo_env_free (env);

  return NULL;
}

/*
 * qo_optimize_helper () -
 *   return:
 *   env(in):
 */
static QO_PLAN *
qo_optimize_helper (QO_ENV * env)
{
  PARSER_CONTEXT *parser;
  PT_NODE *tree;
  PT_NODE *spec, *conj, *next;
  QO_ENV *local_env;		/* So we can safely take its address */
  QO_PLAN *plan;
  int level;
  QO_TERM *term;
  QO_NODE *node, *p_node;
  BITSET nodeset;
  int n;

  parser = QO_ENV_PARSER (env);
  tree = QO_ENV_PT_TREE (env);
  local_env = env;

  (void) parser_walk_tree (parser, tree, build_query_graph, &local_env,
			   build_query_graph_post, &local_env);
  (void) add_hint (env, tree);
  add_using_index (env, tree->info.query.q.select.using_index);

  /* add dep term */
  {
    BITSET dependencies;
    BITSET antecedents;

    bitset_init (&dependencies, env);
    bitset_init (&antecedents, env);

    for (n = 0; n < env->nnodes; n++)
      {
	node = QO_ENV_NODE (env, n);
	spec = QO_NODE_ENTITY_SPEC (node);

	/*
	 * Set up the dependencies; it's simplest just to assume that a
	 * dependent table depends on everything that precedes it.
	 */
	if (is_dependent_table (spec))
	  {
	    /*
	     * Find all of the segments (i.e., attributes) referenced in
	     * the derived table expression, and then find the set of
	     * nodes that underly those segments.  This node can't be
	     * added to a join plan before all of those nodes have, so we
	     * establish some artificial dependency links that force the
	     * planner to maintain that constraint.
	     */

	    BITSET_CLEAR (dependencies);
	    BITSET_CLEAR (antecedents);

	    qo_expr_segs (env, spec->info.spec.derived_table, &dependencies);

	    if (!bitset_is_empty (&dependencies))
	      {
		qo_seg_nodes (env, &dependencies, &antecedents);
		qo_add_dep_term (node, &antecedents, &dependencies, env);
	      }
	  }
      }				/* for (n = 0 ...) */

    bitset_delset (&dependencies);
    bitset_delset (&antecedents);
  }

  bitset_init (&nodeset, env);
  /* add term in the ON clause */
  for (spec = tree->info.query.q.select.from; spec; spec = spec->next)
    {
      if (spec->node_type == PT_SPEC && spec->info.spec.on_cond)
	{
	  for (conj = spec->info.spec.on_cond; conj; conj = conj->next)
	    {
	      next = conj->next;
	      conj->next = NULL;

	      /* The conjuct could be PT_VALUE(0) if an explicit join
	         condition was derived/transformed to the always-false
	         search condition when type checking, expression evaluation
	         or query rewrite transformation. We should sustained it for
	         correct join plan. It's different from ordinary WHERE search
	         condition. If an always-false search condition was found
	         in WHERE clause, they did not call query optimization and
	         return no result to the user unless the query doesn't have
	         aggregation. */

	      term = qo_add_term (conj, PREDICATE_TERM, env);

	      if (QO_TERM_CLASS (term) == QO_TC_JOIN)
		{
		  n = QO_TERM_LOCATION (term);
		  if (QO_NODE_LOCATION (QO_TERM_HEAD (term)) == n - 1
		      && QO_NODE_LOCATION (QO_TERM_TAIL (term)) == n)
		    {
		      bitset_add (&nodeset,
				  QO_NODE_IDX (QO_TERM_TAIL (term)));
		    }
		}

	      conj->next = next;
	    }
	}
    }

  /* add term in the WHERE clause */
  for (conj = tree->info.query.q.select.where; conj; conj = conj->next)
    {
      next = conj->next;
      conj->next = NULL;

      term = qo_add_term (conj, PREDICATE_TERM, env);
      conj->next = next;
    }

  for (n = 1; n < env->nnodes; n++)
    {
      node = QO_ENV_NODE (env, n);
      spec = QO_NODE_ENTITY_SPEC (node);
      /* check join-edge for explicit join */
      if (spec->node_type == PT_SPEC
	  && (spec->info.spec.join_type == PT_JOIN_INNER
	      || spec->info.spec.join_type == PT_JOIN_LEFT_OUTER
	      || spec->info.spec.join_type == PT_JOIN_RIGHT_OUTER)
	  && !BITSET_MEMBER (nodeset, n))
	{
	  p_node = QO_ENV_NODE (env, n - 1);
	  (void) qo_add_dummy_join_term (env, p_node, node);
	  /* Is it safe to pass node[n-1] as head node?
	     Yes, because the sequence of QO_NODEs corresponds to
	     the sequence of PT_SPEC list */
	}
    }

  bitset_delset (&nodeset);

  (void) parser_walk_tree (parser, tree->info.query.q.select.list,
			   qo_add_final_segment, &local_env, pt_continue_walk,
			   NULL);
  (void) parser_walk_tree (parser, tree->info.query.q.select.group_by,
			   qo_add_final_segment, &local_env, pt_continue_walk,
			   NULL);
  (void) parser_walk_tree (parser, tree->info.query.q.select.having,
			   qo_add_final_segment, &local_env, pt_continue_walk,
			   NULL);

  /* finish the rest of the opt structures */
  qo_discover_edges (env);

  /*
   * Don't do these things until *after* qo_discover_edges(); that
   * function may rearrange the QO_TERM structures that were discovered
   * during the earlier phases, and anyone who grabs the idx of one of
   * the terms (or even a pointer to one) will be pointing to the wrong
   * term after they're rearranged.
   */
  qo_assign_eq_classes (env);
  get_local_subqueries (env, tree);
  get_rank (env);
  qo_discover_indexes (env);
  qo_discover_partitions (env);

  /* now optimize */

  plan = qo_planner_search (env);

  /* need to set est_card for the select in case it is a subquery */

  /*
   * Print out any needed post-optimization info.  Leave a way to find
   * out about environment info if we aren't able to produce a plan.
   * If this happens in the field at least we'll be able to glean some
   * info.
   */
  qo_get_optimization_param (&level, QO_PARAM_LEVEL);
  if (PLAN_DUMP_ENABLED (level) && DETAILED_DUMP (level))
    {
      qo_env_dump (env, query_plan_dump_fp);
    }
  if (plan == NULL)
    {
      qo_env_free (env);
    }

  return plan;

}

/*
 * qo_env_init () - initialize an optimizer environment
 *   return: QO_ENV *
 *   parser(in): parser environment
 *   query(in): A pointer to a PT_NODE structure that describes the query
 *		to be optimized
 */
static QO_ENV *
qo_env_init (PARSER_CONTEXT * parser, PT_NODE * query)
{
  QO_ENV *env;
  int i;

  if (query == NULL)
    {
      return NULL;
    }

  env = qo_env_new (parser, query);
  if (env == NULL)
    {
      return NULL;
    }

  if (qo_validate (env))
    {
      goto error;
    }

  env->segs = NALLOCATE (env, QO_SEGMENT, env->nsegs);
  env->nodes = NALLOCATE (env, QO_NODE, env->nnodes);
  env->terms = NALLOCATE (env, QO_TERM, env->nterms);
  env->eqclasses = NALLOCATE (env, QO_EQCLASS,
			      MAX (env->nnodes, env->nterms) + env->nsegs);
  env->partitions = NALLOCATE (env, QO_PARTITION, env->nnodes);

  if (env->segs == NULL
      || env->nodes == NULL
      || (env->nterms && env->terms == NULL)
      || env->eqclasses == NULL || env->partitions == NULL)
    {
      goto error;
    }

  for (i = 0; i < env->nsegs; ++i)
    {
      qo_seg_clear (env, i);
    }
  for (i = 0; i < env->nnodes; ++i)
    {
      qo_node_clear (env, i);
    }
  for (i = 0; i < env->nterms; ++i)
    {
      qo_term_clear (env, i);
    }

  env->Nnodes = env->nnodes;
  env->Nsegs = env->nsegs;
  env->Nterms = env->nterms;
  env->Neqclasses = MAX (env->nnodes, env->nterms) + env->nsegs;

  env->nnodes = 0;
  env->nsegs = 0;
  env->nterms = 0;
  env->neqclasses = 0;

  QO_INFINITY = infinity ();

  return env;

error:
  qo_env_free (env);
  return NULL;

}



/*
 * qo_validate () -
 *   return: true iff we reject the query, false otherwise
 *   env(in): A pointer to the environment we are working on
 *
 * Note: Determine whether this is a problem that we're willing to
 *	work on.  Right now, this means enforcing the constraints
 *	about maximum set sizes.  We're not very happy with
 *	set-valued attributes, class attributes, or shared attributes
 *      either, but these are temporary problems and are detected
 *      elsewhere.
 */
static bool
qo_validate (QO_ENV * env)
{
#define OPTIMIZATION_LIMIT      64
  PT_NODE *tree, *spec, *conj;

  tree = QO_ENV_PT_TREE (env);

  /* find out how many nodes and segments will be required for the
   * query graph.
   */
  (void) parser_walk_tree (env->parser, tree, graph_size_select, &env,
			   pt_continue_walk, NULL);

  /* count the number of conjuncts in the ON clause */
  for (spec = tree->info.query.q.select.from; spec; spec = spec->next)
    {
      if (spec->node_type == PT_SPEC && spec->info.spec.on_cond)
	{
	  for (conj = spec->info.spec.on_cond; conj; conj = conj->next)
	    {
	      if (conj->node_type != PT_EXPR && conj->node_type != PT_VALUE)
		{		/* for outer join */
		  env->bail_out = 1;
		  return true;
		}
	      env->nterms++;
	    }
	}
    }

  /* count the number of conjuncts in the WHERE clause */
  for (conj = tree->info.query.q.select.where; conj; conj = conj->next)
    {
      if (conj->node_type != PT_EXPR
	  && conj->node_type != PT_VALUE /* is a false conjunct */ )
	{
	  env->bail_out = 1;
	  return true;
	}
      env->nterms++;
    }

  if (env->nnodes > OPTIMIZATION_LIMIT)
    {
      return true;
    }

  return false;

}

/*
 * graph_size_select () - This pre walk function will examine the current
 *			  select and determine the graph size needed for it
 *   return: PT_NODE *
 *   parser(in): parser environment
 *   tree(in): tree to walk
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
graph_size_select (PARSER_CONTEXT * parser, PT_NODE * tree, void *arg,
		   int *continue_walk)
{
  QO_ENV *env = (QO_ENV *) * (long *) arg;

  *continue_walk = PT_CONTINUE_WALK;

  /*
   * skip nested selects, they've already been counted
   */
  if ((tree->node_type == PT_SELECT) && (tree != env->pt_tree))
    {
      *continue_walk = PT_LIST_WALK;
      return tree;
    }

  /* if its not an entity_spec, we're not interested */
  if (tree->node_type != PT_SPEC)
    {
      return tree;
    }

  (void) graph_size_for_entity (env, tree);

  /* don't visit leaves of the entity, graph_size_for_entity already did */
  *continue_walk = PT_LIST_WALK;

  return tree;

}

/*
 * graph_size_for_entity () - This routine will size the graph for the entity
 *   return: nothing
 *   env(in): optimizer environment
 *   entity(in): entity to build the graph for
 *
 * Note: This routine mimics build_graph_for_entity.  It is IMPORTANT that
 *      they remain in sync or else we might not allocate enough space for
 *      the graph arrays resulting in memory corruption.
 */
static void
graph_size_for_entity (QO_ENV * env, PT_NODE * entity)
{
  PT_NODE *name, *conj, *next_entity;

  env->nnodes++;

  /* create oid segment for the entity */
  env->nsegs++;

  for (name = get_referenced_attrs (entity); name != NULL; name = name->next)
    {
      env->nsegs++;
    }

  if (is_dependent_table (entity))
    {
      env->nterms += env->nnodes;
    }

  /* recurse and size the graph for path entities */
  for (next_entity = entity->info.spec.path_entities;
       next_entity != NULL; next_entity = next_entity->next)
    {
      (void) graph_size_for_entity (env, next_entity);
    }

  /* create a term for each conjunct in the entity's path_conjuncts */
  for (conj = entity->info.spec.path_conjuncts;
       conj != NULL; conj = conj->next)
    {
      env->nterms++;
    }

  /* reserve space for explicit join dummy term */
  switch (entity->info.spec.join_type)
    {
    case PT_JOIN_INNER:
      /* reserve dummy inner join term */
      env->nterms++;
      /* reserve additional always-false sarg */
      env->nterms++;
      break;
    case PT_JOIN_LEFT_OUTER:
    case PT_JOIN_RIGHT_OUTER:
    case PT_JOIN_FULL_OUTER:
      /* reserve dummy outer join term */
      env->nterms++;
      break;
    default:
      break;
    }

}

/*
 * build_query_graph () - This pre walk function will build the portion of the
 *			  query graph for each entity in the entity_list
 *			  (from list)
 *   return: PT_NODE *
 *   parser(in): parser environment
 *   tree(in): tree to walk
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
build_query_graph (PARSER_CONTEXT * parser, PT_NODE * tree, void *arg,
		   int *continue_walk)
{
  QO_ENV *env = (QO_ENV *) * (long *) arg;

  *continue_walk = PT_CONTINUE_WALK;

  /*
   * skip nested selects, they've already been done and are
   * constant with respect to the current select statement
   */
  if ((tree->node_type == PT_SELECT) && (tree != env->pt_tree))
    {
      *continue_walk = PT_LIST_WALK;
      return tree;
    }

  /* if its not an entity_spec, we're not interested */
  if (tree->node_type != PT_SPEC)
    {
      return tree;
    }

  (void) build_graph_for_entity (env, tree, QO_BUILD_ENTITY);

  /* don't visit leaves of the entity, build_graph_for_entity already did */
  *continue_walk = PT_LIST_WALK;

  return tree;
}

/*
 * build_query_graph_post () - This post walk function will build the portion
 *			       of the query graph for each path-entity in the
 *			       entity_list (from list)
 *   return:
 *   parser(in): parser environment
 *   tree(in): tree to walk
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
build_query_graph_post (PARSER_CONTEXT * parser, PT_NODE * tree, void *arg,
			int *continue_walk)
{
  QO_ENV *env = (QO_ENV *) * (long *) arg;

  *continue_walk = PT_CONTINUE_WALK;

  /* if its not an entity_spec, we're not interested */
  if (tree->node_type != PT_SPEC)
    {
      return tree;
    }

  (void) build_graph_for_entity (env, tree, QO_BUILD_PATH);

  return tree;
}

/*
 * build_graph_for_entity () - This routine will create nodes and segments
 *			       based on the parse tree entity
 *   return: QO_NODE *
 *   env(in): optimizer environment
 *   entity(in): entity to build the graph for
 *   status(in):
 *
 * Note: Any changes made to this routine should be reflected in
 *   graph_size_for_entity.  They must remain in sync or else we might not
 *   allocate enough space for the graph arrays resulting in memory
 *   corruption.
 */
static QO_NODE *
build_graph_for_entity (QO_ENV * env, PT_NODE * entity,
			QO_BUILD_STATUS status)
{
  PARSER_CONTEXT *parser;
  QO_NODE *node = NULL, *next_node;
  PT_NODE *name, *next_entity, *attr, *attr_list;
  QO_SEGMENT *seg;
  parser = QO_ENV_PARSER (env);

  if (!(status & QO_BUILD_ENTITY))
    {
      int i;
      for (i = 0; i < env->nnodes; i++)
	{
	  node = QO_ENV_NODE (env, i);
	  if (QO_NODE_ENTITY_SPEC (node) == entity)
	    {
	      break;		/* found the node */
	    }
	}

      goto build_path;
    }

  node = qo_add_node (entity, env);

  attr_list = get_referenced_attrs (entity);

  /*
   * Find the PT_NAME corresponding to this entity spec (i.e., the
   * PT_NODE that we want backing the oid segment we're about to
   * create), if such exists.
   */

  for (attr = attr_list; attr && !IS_OID (attr); attr = attr->next)
    {
      ;
    }

  /*
   * 'attr' will be non-null iff the oid "attribute" of the class
   * is explicitly used in some way, e.g., in a comparison or a projection.
   * If it is non-null, it will be created with the rest of the symbols.
   *
   * If it is null, we'll make one unless we're dealing with a derived
   * table.
   */
  if (!attr && !entity->info.spec.derived_table)
    {
      attr = parser_new_node (parser, PT_NAME);
      attr->info.name.resolved =
	entity->info.spec.flat_entity_list->info.name.original;
      attr->info.name.original = "";
      attr->info.name.spec_id = entity->info.spec.id;
      attr->info.name.meta_class = PT_OID_ATTR;

      /* create oid segment for the entity */
      seg = qo_insert_segment (node, NULL, attr, env);
      QO_SEG_SET_VALUED (seg) = false;	/* oid segments aren't set valued */
      QO_SEG_CLASS_ATTR (seg) = false;	/* oid segments aren't class attrs */
      QO_SEG_SHARED_ATTR (seg) = false;	/* oid segments aren't shared attrs */
    }

  /*
   * Create a segment for each symbol in the entities symbol table.
   */
  for (name = attr_list; name != NULL; name = name->next)
    {
      seg = qo_insert_segment (node, NULL, name, env);

      if ((name->type_enum == PT_TYPE_SET) ||
	  (name->type_enum == PT_TYPE_MULTISET) ||
	  (name->type_enum == PT_TYPE_SEQUENCE))
	{
	  QO_SEG_SET_VALUED (seg) = true;
	}
      else
	{
	  QO_SEG_SET_VALUED (seg) = false;
	}

      if (name->info.name.meta_class == PT_META_ATTR)
	{
	  QO_SEG_CLASS_ATTR (seg) = true;
	}
      else
	{
	  QO_SEG_CLASS_ATTR (seg) = false;
	}

      /* this needs to check a flag Bill is going to add--CHECK!!!!! */
      QO_SEG_SHARED_ATTR (seg) = false;
    }

build_path:

  if (!(status & QO_BUILD_PATH))
    {
      return node;
    }

  /* recurse and build the graph for path entities */
  for (next_entity = entity->info.spec.path_entities;
       next_entity != NULL; next_entity = next_entity->next)
    {
      next_node = build_graph_for_entity (env, next_entity,
					  (QO_BUILD_STATUS) (QO_BUILD_ENTITY |
							     QO_BUILD_PATH));

      /* for each path entity, fix the join segment */
      QO_ASSERT (env, next_node != NULL);

      /* make sure path entity contains the one and only path conjunct */
      QO_ASSERT (env, next_entity->info.spec.path_conjuncts != NULL);
      QO_ASSERT (env, next_entity->info.spec.path_conjuncts->next == NULL);

      (void) qo_join_segment (node, next_node, next_entity->info.spec.
			      path_conjuncts->info.expr.arg1, env);
    }

  /* create a term for the entity's path_conjunct if one exists */
  if (entity->info.spec.path_conjuncts != NULL)
    {
      (void) qo_add_term (entity->info.spec.path_conjuncts,
			  entity->info.spec.meta_class, env);
    }

  return node;
}

/*
 * qo_add_node () - This routine adds a node to the optimizer environment
 *		 for the entity
 *   return: QO_NODE *
 *   entity(in): entity to add node for
 *   env(in): optimizer environment
 */
static QO_NODE *
qo_add_node (PT_NODE * entity, QO_ENV * env)
{
  QO_NODE *node = NULL;
  QO_CLASS_INFO *info;
  int i, n;

  QO_ASSERT (env, env->nnodes < env->Nnodes);

  node = QO_ENV_NODE (env, env->nnodes);

  /* fill in node */
  QO_NODE_ENV (node) = env;
  QO_NODE_ENTITY_SPEC (node) = entity;
  QO_NODE_NAME (node) = entity->info.spec.range_var->info.name.original;
  QO_NODE_IDX (node) = env->nnodes;
  env->nnodes++;

  /*
   * If derived table there will be no info.  Also if derived table
   * that is correlated to the current scope level, establish
   * dependency links to all nodes that precede it in the scope.  This
   * is overkill, but it's easier than figuring out the exact
   * information, and it's usually the same anyway.
   */
  if (entity->info.spec.derived_table == NULL
      && (info = qo_get_class_info (env, node)) != NULL)
    {
      QO_NODE_INFO (node) = info;
      for (i = 0, n = info->n; i < n; i++)
	{
	  QO_ASSERT (env, QO_GET_CLASS_STATS (&info->info[i]) != NULL);
	  if (entity->info.spec.meta_class == PT_META_CLASS)
	    {
	      /* is class OID reference spec
	       * for example: 'class x'
	       *   SELECT class_meth(class x, x.i) FROM x, class x
	       */
	      QO_NODE_NCARD (node) += 1;
	      QO_NODE_TCARD (node) += 1;
	    }
	  else
	    {
	      QO_NODE_NCARD (node) +=
		QO_GET_CLASS_STATS (&info->info[i])->num_objects;
	      QO_NODE_TCARD (node) +=
		QO_GET_CLASS_STATS (&info->info[i])->heap_size;
	    }
	}			/* for (i = ... ) */
    }
  else
    {
      QO_NODE_NCARD (node) = 5;	/* just guess */
      QO_NODE_TCARD (node) = 1;	/* just guess */

      /* recalculate derived table size */
      if (entity->info.spec.derived_table)
	{
	  XASL_NODE *xasl;

	  switch (entity->info.spec.derived_table->node_type)
	    {
	    case PT_SELECT:
	    case PT_UNION:
	    case PT_DIFFERENCE:
	    case PT_INTERSECTION:
	      xasl =
		(XASL_NODE *) entity->info.spec.derived_table->info.query.
		xasl;
	      if (xasl)
		{
		  QO_NODE_NCARD (node) = (unsigned long) xasl->cardinality;
		  QO_NODE_TCARD (node) = (unsigned long)
		    ((QO_NODE_NCARD (node) *
		      (double) xasl->projected_size) / (double) IO_PAGESIZE);
		  if (QO_NODE_TCARD (node) == 0)
		    {
		      QO_NODE_TCARD (node) = 1;
		    }
		}
	      break;
	    default:
	      break;
	    }
	}
    }

  n = QO_NODE_IDX (node);
  QO_NODE_SARGABLE (node) = true;
  switch (QO_NODE_PT_JOIN_TYPE (node))
    {
    case PT_JOIN_LEFT_OUTER:
      {
	bool found_prev_not_sargable;
	QO_NODE *prev;

	found_prev_not_sargable = false;	/* init */
	for (i = n - 1; i > 0 && !found_prev_not_sargable; i--)
	  {
	    prev = QO_ENV_NODE (env, i);
	    /* directly outer-join connected */
	    if (QO_NODE_PT_JOIN_TYPE (prev) == PT_JOIN_LEFT_OUTER
		|| QO_NODE_PT_JOIN_TYPE (prev) == PT_JOIN_RIGHT_OUTER
		|| QO_NODE_PT_JOIN_TYPE (prev) == PT_JOIN_FULL_OUTER)
	      {
		if (!QO_NODE_SARGABLE (QO_ENV_NODE (env, i - 1)))
		  {
		    found_prev_not_sargable = true;
		  }
	      }
	    else
	      {			/* not directly outer-join connected */
		break;		/* give up */
	      }
	  }			/* for (i = n - 1; ...) */

	if (!found_prev_not_sargable)
	  {
	    QO_NODE_SARGABLE (QO_ENV_NODE (env, n - 1)) = false;
	  }
      }
      break;

    case PT_JOIN_RIGHT_OUTER:
      QO_NODE_SARGABLE (node) = false;
      break;

/* currently, not used */
/*    case PT_JOIN_FULL_OUTER:
        QO_NODE_SARGABLE(QO_ENV_NODE(env, n - 1)) = false;
        QO_NODE_SARGABLE(node) = false;
        break;
 */
    default:
      break;
    }

  return node;
}

/*
 * lookup_node () - looks up node in the node array, returns NULL if not found
 *   return: Ptr to node in node table. If node is found, entity will be set
 *	     to point to the corresponding entity spec in the parse tree.
 *   attr(in): class to look up
 *   env(in): optimizer environment
 *   entity(in): entity spec for the node
 */
QO_NODE *
lookup_node (PT_NODE * attr, QO_ENV * env, PT_NODE ** entity)
{
  int i;
  bool found = false;

  QO_ASSERT (env, attr->node_type == PT_NAME);

  for (i = 0; (!found) && (i < env->nnodes); /* no increment step */ )
    {
      *entity = QO_NODE_ENTITY_SPEC (QO_ENV_NODE (env, i));
      if ((*entity)->info.spec.id == attr->info.name.spec_id)
	{
	  found = true;
	}
      else
	{
	  i++;
	}
    }

  return ((found) ? QO_ENV_NODE (env, i) : NULL);
}

/*
 * qo_insert_segment () - inserts a segment into the optimizer environment
 *   return: QO_SEGMENT *
 *   head(in): head of the segment
 *   tail(in): tail of the segment
 *   node(in): pt_node that gave rise to this segment
 *   env(in): optimizer environment
 */
static QO_SEGMENT *
qo_insert_segment (QO_NODE * head, QO_NODE * tail, PT_NODE * node,
		   QO_ENV * env)
{
  QO_SEGMENT *seg = NULL;

  QO_ASSERT (env, head != NULL);
  QO_ASSERT (env, env->nsegs < env->Nsegs);

  seg = QO_ENV_SEG (env, env->nsegs);

  /* fill in seg */
  QO_SEG_PT_NODE (seg) = node;
  QO_SEG_HEAD (seg) = head;
  QO_SEG_TAIL (seg) = tail;
  QO_SEG_IDX (seg) = env->nsegs;
  /* add dummy name to segment
   * example: dummy attr from view transfrom
   *   select count(*) from v
   *   select count(*) from (select {v}, 1 from t) v (v, 1)
   *
   * here, '1' is dummy attr
   * set empty string to avoid core crash
   */
  QO_SEG_NAME (seg) = node->info.name.original ?
    node->info.name.original :
    pt_append_string (QO_ENV_PARSER (env), NULL, "");

  if (IS_OID (node))
    {
      /* this is an oid segment */
      QO_NODE_OID_SEG (head) = seg;
      QO_SEG_INFO (seg) = NULL;
    }
  else if (!PT_IS_CLASSOID_NAME (node))
    {
      /* Ignore CLASSOIDs.  They are generated by updates on the server
       * and can be treated as any other projected column.  We don't
       * need to know anything else about this attr since it can not
       * be used as an index or in any other interesting way.
       */
      QO_SEG_INFO (seg) = qo_get_attr_info (env, seg);
    }

  bitset_add (&(QO_NODE_SEGS (head)), QO_SEG_IDX (seg));

  env->nsegs++;

  return seg;
}

/*
 * qo_join_segment () - This routine will look for the segment and set its tail
 *		     to the correct node
 *   return: QO_SEGMENT *
 *   head(in): head of join segment
 *   tail(in): tail of join segment
 *   name(in): name of join segment
 *   env(in): optimizer environment
 */
static QO_SEGMENT *
qo_join_segment (QO_NODE * head, QO_NODE * tail, PT_NODE * name, QO_ENV * env)
{
  QO_SEGMENT *seg = NULL;

  seg = lookup_seg (head, name, env);
  QO_ASSERT (env, seg != NULL);

  QO_SEG_TAIL (seg) = tail;	/* may be redundant */

  return seg;
}

/*
 * lookup_seg () -
 *   return: ptr to segment in segment table, or NULL if the segment is not
 *	     in the table
 *   head(in): head of the segment
 *   name(in): name of the segment
 *   env(in): optimizer environment
 */
QO_SEGMENT *
lookup_seg (QO_NODE * head, PT_NODE * name, QO_ENV * env)
{
  int i;
  bool found = false;

  for (i = 0; (!found) && (i < env->nsegs); /* no increment step */ )
    {
      if (QO_SEG_HEAD (QO_ENV_SEG (env, i)) == head
	  && pt_name_equal (QO_ENV_PARSER (env),
			    QO_SEG_PT_NODE (QO_ENV_SEG (env, i)), name))
	{
	  found = true;
	}
      else
	{
	  i++;
	}
    }

  return ((found) ? QO_ENV_SEG (env, i) : NULL);
}

/*
 * qo_add_final_segment () -
 *   return: PT_NODE *
 *   parser(in): parser environment
 *   tree(in): tree to walk
 *   arg(in):
 *   continue_walk(in):
 *
 * Note: This walk "pre" function looks up the segment for each
 *      node in the list.  If the node is a PT_NAME node, it can use it to
 *      find the final segment.  If the node is a dot expression, the final
 *      segment will be the segment associated with the PT_NAME node that is
 *      arg2 of the dot expression.
 */
static PT_NODE *
qo_add_final_segment (PARSER_CONTEXT * parser, PT_NODE * tree, void *arg,
		      int *continue_walk)
{
  QO_ENV *env = (QO_ENV *) * (long *) arg;

  *continue_walk = PT_CONTINUE_WALK;

  if (tree->node_type == PT_NAME)
    {
      (void) set_seg_node (tree, env, &env->final_segs);
      *continue_walk = PT_LIST_WALK;
    }
  else if ((tree->node_type == PT_DOT_))
    {
      (void) set_seg_node (tree->info.dot.arg2, env, &env->final_segs);
      *continue_walk = PT_LIST_WALK;
    }

  return tree;			/* don't alter tree structure */
}

/*
 * qo_add_term () - Creates a new term in the env term table
 *   return: void
 *   conjunct(in): term to add
 *   term_type(in): is the term a path term?
 *   env(in): optimizer environment
 */
static QO_TERM *
qo_add_term (PT_NODE * conjunct, int term_type, QO_ENV * env)
{
  QO_TERM *term;
  QO_NODE *node;

  /* The conjuct could be PT_VALUE(0);
     (1) if an outer join condition was derived/transformed to the
     always-false ON condition when type checking, expression evaluation
     or query rewrite transformation. We should sustained it for correct
     outer join plan. It's different from ordinary WHERE condition.
     (2) Or is an always-false WHERE condition */
  QO_ASSERT (env, conjunct->node_type == PT_EXPR ||
	     conjunct->node_type == PT_VALUE);
  QO_ASSERT (env, env->nterms < env->Nterms);

  term = QO_ENV_TERM (env, env->nterms);

  /* fill in term */
  QO_TERM_CLASS (term) = QO_TC_SARG;	/* assume sarg until proven otherwise */
  QO_TERM_JOIN_TYPE (term) = NO_JOIN;
  QO_TERM_PT_EXPR (term) = conjunct;
  QO_TERM_LOCATION (term) = (conjunct->node_type == PT_EXPR ?
			     conjunct->info.expr.location :
			     conjunct->info.value.location);
  QO_TERM_SELECTIVITY (term) = 0.0;
  QO_TERM_RANK (term) = 0;
  QO_TERM_FLAG (term) = 0;	/* init */
  QO_TERM_IDX (term) = env->nterms;

  env->nterms++;

  if (conjunct->node_type == PT_EXPR)
    {
      (void) qo_analyze_term (term, term_type);
    }
  else
    {
      /* conjunct->node_type == PT_VALUE */
      if (conjunct->info.value.location == 0)
	{
	  /* is an always-false WHERE condition */
	  QO_TERM_CLASS (term) = QO_TC_OTHER;	/* is dummy */
	}
      else
	{
	  /* Assume 'conjunct->info.value.location' is same to QO_NODE idx */
	  node = QO_ENV_NODE (env, conjunct->info.value.location);
	  switch (QO_NODE_PT_JOIN_TYPE (node))
	    {
	    case PT_JOIN_INNER:
	      /* add always-false arg to each X, Y
	       * example: SELECT ... FROM X inner join Y on 0 <> 0;
	       */
	      bitset_add (&(QO_TERM_NODES (term)), QO_NODE_IDX (node) - 1);

	      term = QO_ENV_TERM (env, env->nterms);

	      /* fill in term */
	      QO_TERM_CLASS (term) = QO_TC_SARG;
	      QO_TERM_JOIN_TYPE (term) = NO_JOIN;
	      QO_TERM_PT_EXPR (term) = conjunct;
	      QO_TERM_LOCATION (term) = conjunct->info.value.location;
	      QO_TERM_SELECTIVITY (term) = 0.0;
	      QO_TERM_RANK (term) = 0;
	      QO_TERM_FLAG (term) = 0;	/* init */
	      QO_TERM_IDX (term) = env->nterms;

	      env->nterms++;

	      bitset_add (&(QO_TERM_NODES (term)), QO_NODE_IDX (node));
	      break;
	    case PT_JOIN_LEFT_OUTER:
	      bitset_add (&(QO_TERM_NODES (term)), QO_NODE_IDX (node));
	      break;
	    case PT_JOIN_RIGHT_OUTER:
	      bitset_add (&(QO_TERM_NODES (term)), QO_NODE_IDX (node) - 1);
	      break;
	    case PT_JOIN_FULL_OUTER:
	      /* I don't know what is to be done for full outer. */
	      break;
	    default:
	      /* this should not happen */
	      bitset_add (&(QO_TERM_NODES (term)), QO_NODE_IDX (node));
	      break;
	    }
	}			/* else */
    }

  return term;
}

/*
 * qo_add_dep_term () -
 *   return: void
 *   derived_node(in): The node representing the dependent derived table
 *   depend_nodes(in):
 *   depend_segs(in):
 *   env(in): optimizer environment
 *
 * Note: Creates a new QO_TC_DEP_LINK term in the env term table, plus
 *	QO_TC_DEP_JOIN terms as necessary.  QO_TC_DEP_LINK terms are
 *	used only to capture dependency information between a node
 *	representing a dependent derived table and a node on which
 *	that derived table depends.
 */
static void
qo_add_dep_term (QO_NODE * derived_node,
		 BITSET * depend_nodes, BITSET * depend_segs, QO_ENV * env)
{
  QO_TERM *term = NULL;
  BITSET_ITERATOR bi;
  int ni, di;

  QO_ASSERT (env, env->nterms < env->Nterms);

  term = QO_ENV_TERM (env, env->nterms);

  bitset_assign (&(QO_NODE_DEP_SET (derived_node)), depend_nodes);

  /* fill in term */
  QO_TERM_CLASS (term) = QO_TC_DEP_LINK;
  QO_TERM_PT_EXPR (term) = NULL;
  QO_TERM_LOCATION (term) = 0;
  QO_TERM_SELECTIVITY (term) = 1.0;
  QO_TERM_RANK (term) = 0;
  QO_TERM_FLAG (term) = 0;
  QO_TERM_IDX (term) = env->nterms;
  QO_TERM_JOIN_TYPE (term) = JOIN_INNER;
  /*
   * This is misleading if |depend_nodes| > 1, but the planner is the
   * only party relying on this information, and it understands the
   * rules of the game.
   */
  QO_TERM_HEAD (term) = QO_ENV_NODE (env, bitset_first_member (depend_nodes));
  QO_TERM_TAIL (term) = derived_node;

  bitset_assign (&(QO_TERM_NODES (term)), depend_nodes);
  bitset_add (&(QO_TERM_NODES (term)), QO_NODE_IDX (derived_node));
  bitset_assign (&(QO_TERM_SEGS (term)), depend_segs);
  /*
   * Add this term to env->fake_terms so that we're not tempted to sarg
   * it if a mergeable join term between these nodes is also present.
   * This is part of the fix for PR 7314.
   */
  bitset_add (&(env->fake_terms), QO_TERM_IDX (term));

  env->nterms++;

  ni = bitset_iterate (depend_nodes, &bi);
  while ((di = bitset_next_member (&bi)) != -1)
    {
      QO_ASSERT (env, env->nterms < env->Nterms);
      term = QO_ENV_TERM (env, env->nterms);
      QO_TERM_CLASS (term) = QO_TC_DEP_JOIN;
      QO_TERM_PT_EXPR (term) = NULL;
      QO_TERM_SELECTIVITY (term) = 1.0;
      QO_TERM_RANK (term) = 0;
      QO_TERM_FLAG (term) = 0;
      QO_TERM_IDX (term) = env->nterms;
      bitset_add (&(QO_TERM_NODES (term)), ni);
      bitset_add (&(QO_TERM_NODES (term)), di);
      QO_TERM_HEAD (term) = QO_ENV_NODE (env, ni);
      QO_TERM_TAIL (term) = QO_ENV_NODE (env, di);
      QO_TERM_JOIN_TYPE (term) = JOIN_INNER;
      /*
       * Do NOT add these terms to env->fake_terms, because (unlike the
       * DEP_LINK terms) there is no restriction on how they can be
       * implemented (e.g., if there's a mergeable term available to
       * join the two nodes, it's ok to use it).
       */
      env->nterms++;
    }

}

/*
 * qo_add_dummy_join_term () - Make and add dummy join term if there's no explicit
 *			    join term related with given two nodes
 *   return: void
 *   env(in): optimizer environment
 *   head_node(in):
 *   tail_node(in):
 */
static QO_TERM *
qo_add_dummy_join_term (QO_ENV * env, QO_NODE * head_node,
			QO_NODE * tail_node)
{
  QO_TERM *term;

  QO_ASSERT (env, env->nterms < env->Nterms);

  term = QO_ENV_TERM (env, env->nterms);

  /* fill in term */
  QO_TERM_CLASS (term) = QO_TC_DUMMY_JOIN;
  bitset_add (&(QO_TERM_NODES (term)), QO_NODE_IDX (head_node));
  bitset_add (&(QO_TERM_NODES (term)), QO_NODE_IDX (tail_node));
  QO_TERM_HEAD (term) = head_node;
  QO_TERM_TAIL (term) = tail_node;
  QO_TERM_PT_EXPR (term) = NULL;
  QO_TERM_LOCATION (term) = QO_NODE_LOCATION (tail_node);
  QO_TERM_SELECTIVITY (term) = 1.0;
  QO_TERM_RANK (term) = 0;

  switch (QO_NODE_PT_JOIN_TYPE (tail_node))
    {
    case PT_JOIN_LEFT_OUTER:
      QO_TERM_JOIN_TYPE (term) = JOIN_LEFT;
      break;
    case PT_JOIN_RIGHT_OUTER:
      QO_TERM_JOIN_TYPE (term) = JOIN_RIGHT;
      break;
    case PT_JOIN_FULL_OUTER:
      QO_TERM_JOIN_TYPE (term) = JOIN_OUTER;
      break;
    default:
      /* this should not happen */
      QO_TERM_JOIN_TYPE (term) = JOIN_INNER;
      break;
    }
  QO_TERM_FLAG (term) = 0;
  QO_TERM_IDX (term) = env->nterms;

  env->nterms++;

  /* record outer join dependecy */
  if (QO_OUTER_JOIN_TERM (term))
    {
      QO_NODE *p_node;

      p_node = QO_ENV_NODE (env, QO_NODE_IDX (tail_node) - 1);
      bitset_union (&(QO_NODE_OUTER_DEP_SET (tail_node)),
		    &(QO_NODE_OUTER_DEP_SET (p_node)));
      bitset_add (&(QO_NODE_OUTER_DEP_SET (tail_node)), QO_NODE_IDX (p_node));
    }

  return term;
}

/*
 * qo_analyze_term () - determine the selectivity and class of the given term
 *   return: void
 *   term(in): term to analyze
 *   term_type(in): predicate, path or selector path term
 */
static void
qo_analyze_term (QO_TERM * term, int term_type)
{
  QO_ENV *env;
  PARSER_CONTEXT *parser;
  int merge_applies, lhs_indexable, rhs_indexable;
  PT_NODE *pt_expr, *lhs_expr, *rhs_expr;
  QO_NODE *head_node = NULL, *tail_node = NULL, *on_node;
  QO_SEGMENT *head_seg, *tail_seg;
  BITSET lhs_segs, rhs_segs, lhs_nodes, rhs_nodes;
  BITSET_ITERATOR iter;
  PT_OP_TYPE op_type = PT_AND;
  int i, n, location;
  bool is_outer_on_cond;

  env = QO_TERM_ENV (term);
  parser = QO_ENV_PARSER (env);
  pt_expr = QO_TERM_PT_EXPR (term);
  merge_applies = 1;		/* until proven otherwise */
  lhs_indexable = rhs_indexable = 0;	/* until proven as indexable */
  lhs_expr = rhs_expr = NULL;

  bitset_init (&lhs_segs, env);
  bitset_init (&rhs_segs, env);
  bitset_init (&lhs_nodes, env);
  bitset_init (&rhs_nodes, env);

  if (pt_expr->node_type != PT_EXPR)
    {
      goto wrapup;
    }

  /* only intersting in one predicate term; if 'term' has 'or_next', it was
     derived from OR term */
  if (pt_expr->or_next == NULL)
    {

      switch ((op_type = pt_expr->info.expr.op))
	{

	  /* operaotrs classified as lhs- and rhs-indexable */
	case PT_EQ:
	  QO_TERM_SET_FLAG (term, QO_TERM_EQUAL_OP);
	case PT_LT:
	case PT_LE:
	case PT_GT:
	case PT_GE:
	  /* temporary guess; RHS could be a indexable segment */
	  rhs_indexable = 1;
	  /* no break; fall through */

	  /* operaotrs classified as rhs-indexable */
	case PT_BETWEEN:
	case PT_RANGE:
	case PT_IS_IN:
	case PT_EQ_SOME:
	  /* temporary guess; LHS could be a indexable segment */
	  lhs_indexable = 1;
	  /* no break; fall through */

	  /* operators classified as not-indexable */
	case PT_NOT_BETWEEN:
	case PT_IS_NOT_IN:
	case PT_GE_SOME:
	case PT_GT_SOME:
	case PT_LT_SOME:
	case PT_LE_SOME:
	case PT_EQ_ALL:
	case PT_GE_ALL:
	case PT_GT_ALL:
	case PT_LT_ALL:
	case PT_LE_ALL:
	case PT_NE:
	case PT_SETEQ:
	case PT_SETNEQ:
	case PT_SUPERSETEQ:
	case PT_SUPERSET:
	case PT_SUBSET:
	case PT_SUBSETEQ:
	case PT_NE_SOME:
	case PT_NE_ALL:
	case PT_LIKE:
	case PT_NOT_LIKE:
	  /* RHS of the expression */
	  rhs_expr = pt_expr->info.expr.arg2;
	  /* get segments from RHS of the expression */
	  qo_expr_segs (env, rhs_expr, &rhs_segs);
	  /* no break; fall through */

	case PT_IS_NULL:
	case PT_IS_NOT_NULL:
	case PT_EXISTS:
	  /* LHS of the expression */
	  lhs_expr = pt_expr->info.expr.arg1;
	  /* get segments from LHS of the expression */
	  qo_expr_segs (env, lhs_expr, &lhs_segs);
	  /* now break switch statment */
	  break;

	case PT_OR:
	case PT_NOT:
	  /* get segments from the expression itself */
	  qo_expr_segs (env, pt_expr, &lhs_segs);
	  break;

	  /* the other operators that can not be used as term; error case */
	default:
	  /* stop processing */
	  QO_ABORT (env);

	}			/* switch ((op_type = pt_expr->info.expr.op)) */

    }
  else
    {				/* if (pt_expr->or_next == NULL) */
      /* term that consist of more than one predicates; do same as PT_OR */

      qo_expr_segs (env, pt_expr, &lhs_segs);

    }				/* if (pt_expr->or_next == NULL) */


  /* get nodes from segments */
  qo_seg_nodes (env, &lhs_segs, &lhs_nodes);
  qo_seg_nodes (env, &rhs_segs, &rhs_nodes);


  /* do LHS and RHS of the term belong to the different node? */
  if (!bitset_intersects (&lhs_nodes, &rhs_nodes))
    {
      i = 0;			/* idx of term->index_seg[] array; it shall be 0 or 1 */

      /* There terms look like they might be candidates for implementation
         via indexes. Make sure that they really are candidates.
         IMPORTANT: this is not the final say, since we don't know at this
         point whether indexes actually exist or not. We won't know that
         until a later phase (qo_discover_indexes()). Right now we're just
         determining whether these terms qualify structurally. */

      /* examine if LHS is indexable or not? */

      /* is LHS a type of name(attribute) of local database */
      lhs_indexable &= (lhs_indexable && is_local_name (env, lhs_expr));
      if (lhs_indexable)
	{

	  if (op_type == PT_IS_IN || op_type == PT_EQ_SOME)
	    {
	      /* We have to be careful with this case because
	         "i IN (SELECT ...)"
	         has a special meaning: in this case the select is treated as
	         UNBOX_AS_TABLE instead of the usual UNBOX_AS_VALUE, and we
	         can't use an index even if we want to (because of an XASL
	         deficiency). Because is_pseudo_const() wants to believe that
	         subqueries are pseudo-constants, we have to check for that
	         condition outside of is_pseudo_const(). */
	      switch (rhs_expr->node_type)
		{
		case PT_SELECT:
		case PT_UNION:
		case PT_DIFFERENCE:
		case PT_INTERSECTION:
		  lhs_indexable = 0;
		  break;
		case PT_NAME:
		  if (rhs_expr->info.name.meta_class != PT_PARAMETER
		      && pt_is_set_type (rhs_expr))
		    {
		      lhs_indexable = 0;
		    }
		  break;
		case PT_DOT_:
		  if (pt_is_set_type (rhs_expr))
		    {
		      lhs_indexable = 0;
		    }
		  break;
		default:
		  lhs_indexable &= is_pseudo_const (env, rhs_expr);
		}
	    }
	  else
	    {
	      /* is LHS attribute and is RHS constant value ? */
	      lhs_indexable &= is_pseudo_const (env, rhs_expr);
	    }
	}

      if (lhs_indexable)
	{
	  n = bitset_first_member (&lhs_segs);
	  if (n != -1)
	    {
	      /* record in the term that it has indexable segment as LHS */
	      term->index_seg[i++] = QO_ENV_SEG (env, n);
	    }
	}

      /* examine if LHS is indexable or not? */

      /* is RHS attribute and is LHS constant value ? */
      rhs_indexable &= (rhs_indexable && is_local_name (env, rhs_expr)
			&& is_pseudo_const (env, lhs_expr));
      if (rhs_indexable)
	{
	  if (!lhs_indexable)
	    {
	      op_type = pt_converse_op (op_type);
	      if (op_type != 0)
		{
		  /* converse 'const op attr' to 'attr op const' */
		  PT_NODE *tmp;

		  tmp = pt_expr->info.expr.arg2;
		  pt_expr->info.expr.arg2 = pt_expr->info.expr.arg1;
		  pt_expr->info.expr.arg1 = tmp;
		  pt_expr->info.expr.op = op_type;
		}
	      else
		{
		  /* must be impossible error. check pt_converse_op() */
		  QO_ABORT (env);
		}
	    }

	  n = bitset_first_member (&rhs_segs);
	  if (n != -1)
	    {
	      /* record in the term that it has indexable segment as RHS */
	      term->index_seg[i++] = QO_ENV_SEG (env, n);
	    }
	}			/* if (rhs_indexable) */


      QO_TERM_CAN_USE_INDEX (term) = i;	/* cardinality of term->index_seg[] array */

    }
  else
    {				/* if (!bitset_intersects(&lhs_nodes, &rhs_nodes)) */

      merge_applies = 0;
      QO_TERM_CAN_USE_INDEX (term) = 0;

    }				/* if (!bitset_intersects(&lhs_nodes, &rhs_nodes)) */


  /* fill in segment and node information of QO_TERM structure */
  bitset_assign (&(QO_TERM_SEGS (term)), &lhs_segs);
  bitset_union (&(QO_TERM_SEGS (term)), &rhs_segs);
  bitset_assign (&(QO_TERM_NODES (term)), &lhs_nodes);
  bitset_union (&(QO_TERM_NODES (term)), &rhs_nodes);

  /* location of this term */
  location = QO_TERM_LOCATION (term);
  QO_ASSERT (env, location >= 0);

  is_outer_on_cond = false;	/* init */
  if (location > 0)
    {
      on_node = QO_ENV_NODE (env, location);
      if (QO_NODE_PT_JOIN_TYPE (on_node) == PT_JOIN_LEFT_OUTER
	  || QO_NODE_PT_JOIN_TYPE (on_node) == PT_JOIN_RIGHT_OUTER
	  || QO_NODE_PT_JOIN_TYPE (on_node) == PT_JOIN_FULL_OUTER)
	{
	  is_outer_on_cond = true;
	}
    }

  /* number of nodes with which this term associated */
  n = bitset_cardinality (&(QO_TERM_NODES (term)));
  /* determine the class of the term */
  if (n == 0)
    {
      if (QO_TERM_LOCATION (term) > 0)
	{
	  QO_TERM_CLASS (term) = QO_TC_DURING_JOIN;
	}
      else
	{
	  bool inst_num = false;

	  (void) parser_walk_tree (parser, pt_expr, pt_check_instnum_pre,
				   NULL, pt_check_instnum_post, &inst_num);
	  QO_TERM_CLASS (term) =
	    inst_num ? QO_TC_TOTALLY_AFTER_JOIN : QO_TC_OTHER;
	}
    }
  else
    {
      if (term_type != PREDICATE_TERM)
	{
	  QO_TERM_CLASS (term) = QO_TC_PATH;
	}
      else if (n == 1)
	{
	  QO_TERM_CLASS (term) = QO_TC_SARG;
	}
      else
	{			/* n >= 2 */
	  if (location == 0)
	    {			/* in WHERE condition */
	      QO_TERM_CLASS (term) = QO_TC_OTHER;	/* init */
	      if (n == 2)
		{
		  QO_TERM_CLASS (term) = QO_TC_JOIN;	/* init */
		}
	    }
	  else
	    {			/* in ON condition */
	      on_node = QO_ENV_NODE (env, location);
	      switch (QO_NODE_PT_JOIN_TYPE (on_node))
		{
		case PT_JOIN_LEFT_OUTER:
		case PT_JOIN_RIGHT_OUTER:
		  /*  case PT_JOIN_FULL_OUTER: *//* not used */
		  QO_TERM_CLASS (term) = QO_TC_DURING_JOIN;	/* init */
		  break;

		default:
		  QO_TERM_CLASS (term) = QO_TC_OTHER;	/* init */
		  break;
		}

	      if (n == 2)
		{
		  head_node =
		    QO_ENV_NODE (env,
				 bitset_iterate (&(QO_TERM_NODES (term)),
						 &iter));
		  tail_node = QO_ENV_NODE (env, bitset_next_member (&iter));

		  if ((QO_NODE_IDX (head_node) == QO_NODE_IDX (on_node) - 1
		       && QO_NODE_IDX (tail_node) == QO_NODE_IDX (on_node))
		      || (QO_NODE_IDX (head_node) == QO_NODE_IDX (on_node)
			  && (QO_NODE_IDX (tail_node) ==
			      QO_NODE_IDX (on_node) - 1)))
		    {
		      QO_TERM_CLASS (term) = QO_TC_JOIN;
		    }
		}
	    }
	}
    }

  /* outer join cond shall not be others except QO_TC_SARG and QO_TC_JOIN */
  if (is_outer_on_cond
      && QO_TERM_CLASS (term) != QO_TC_SARG
      && QO_TERM_CLASS (term) != QO_TC_JOIN
      && QO_TERM_CLASS (term) != QO_TC_DURING_JOIN)
    {
      QO_ABORT (env);
    }

  /* re-classify QO_TC_SARG term for outer join */
  if (n == 1)
    {				/* QO_TERM_CLASS(term) == QO_TC_SARG */
      /* QO_NODE to which this sarg term belongs */
      head_node =
	QO_ENV_NODE (env, bitset_first_member (&(QO_TERM_NODES (term))));

      if (location > 0)
	{
	  if (is_outer_on_cond)
	    {
	      /* this term appears in outer join condition of FROM clause */

	      if (!QO_NODE_SARGABLE (head_node))
		{
		  QO_TERM_CLASS (term) = QO_TC_DURING_JOIN;
		}
	      else if (QO_NODE_LOCATION (head_node) < location)
		{
		  PT_NODE *entity;

		  entity = QO_NODE_ENTITY_SPEC (head_node);
		  if (entity->node_type == PT_SPEC
		      && entity->info.spec.on_cond)
		    {
		      /* example: change 'Y.i = 2' to during-join term
		       *  SELECT ...
		       *  FROM X right join Y on ... rigint join Z on Y.i = 2
		       */
		      QO_TERM_CLASS (term) = QO_TC_DURING_JOIN;
		    }
		  else if (QO_NODE_LOCATION (head_node) + 1 < location)
		    {
		      /* example: change 'X.i = 1' to during-join term
		       *  SELECT ...
		       *  FROM X right join Y on ... rigint join Z on X.i = 1
		       */
		      QO_TERM_CLASS (term) = QO_TC_DURING_JOIN;
		    }
		}
	    }			/* if (is_outer_on_cond) */
	}
      else
	{			/* if (location > 0) */
	  int join_idx, node_idx;

	  /* this term appears in search condition of WHERE clause */


	  /* set the start node of outer join - init */
	  join_idx = -1;

	  node_idx = QO_NODE_IDX (head_node);
	  /* if the sarg term belongs to null padding table; */
	  if (QO_NODE_PT_JOIN_TYPE (head_node) == PT_JOIN_LEFT_OUTER)
	    {
	      join_idx = node_idx;	/* case 4.2 */
	    }
	  else
	    {
	      /* NEED MORE OPTIMIZATION for furture */
	      for (node_idx += 1; node_idx < env->nnodes; node_idx++)
		{
		  if (QO_NODE_PT_JOIN_TYPE (QO_ENV_NODE (env, node_idx)) ==
		      PT_JOIN_RIGHT_OUTER)
		    {
		      join_idx = node_idx;	/* case 4.3 */
		      break;
		    }
		}
	    }

	  /* check for the next right outer join;
	     case no.4 of term class in 'outer join TM document' */
	  if (join_idx != -1)
	    {
	      QO_TERM_CLASS (term) = QO_TC_AFTER_JOIN;
	    }
	}			/* if (location > 0) */
    }				/* if (n == 1) */


  if (n == 2)
    {
      /* Although it may be tempting to say that the head node is the first
         member of the lhs_nodes and the tail node is the first member of the
         rhs_nodes, that's not always true. For example, a term like
         "x.a + y.b < 100"
         can get in here, and then *both* head and tail are in lhs_nodes.
         If you get down into the code guarded by 'merge_applies' you can
         safely make more stringent assumptions, but not before then. */
      head_node =
	QO_ENV_NODE (env, bitset_iterate (&(QO_TERM_NODES (term)), &iter));
      tail_node = QO_ENV_NODE (env, bitset_next_member (&iter));

      /* This is a pretty weak test; it only looks for equality
         comparisons. */
      merge_applies &= expr_is_mergable (pt_expr);

      /* check for dependent edge. do not join with it */
      if (BITSET_MEMBER (QO_NODE_DEP_SET (head_node), QO_NODE_IDX (tail_node))
	  || BITSET_MEMBER (QO_NODE_DEP_SET (tail_node),
			    QO_NODE_IDX (head_node)))
	{

	  QO_TERM_CLASS (term) = QO_TC_OTHER;

	  merge_applies = 0;
	}

      /* And there had better be something on both sides of the comparison
         too. You don't want to be misled by something like
         "x.a + y.b = 100"
         because that's definitely not mergeable right now. Perhaps if we
         rewrote it like
         "x.a = 100 - y.b"
         but that seems to be stretching things a little bit. */
      merge_applies &= (!bitset_is_empty (&lhs_segs)
			&& !bitset_is_empty (&rhs_segs));

      if (merge_applies || term_type != PREDICATE_TERM)
	{
	  head_seg =
	    QO_ENV_SEG (env, bitset_iterate (&(QO_TERM_SEGS (term)), &iter));
	  tail_seg = QO_ENV_SEG (env, bitset_next_member (&iter));

	  if (term_type != PREDICATE_TERM)
	    {
	      /* i.e., it's a path term...
	         In this case, it's imperative that we get the head and tail
	         nodes and segs right. Fortunately, in this particular case
	         we can rely on the compiler to produce the term in a
	         consistent way, with the head on the lhs and the tail on
	         the rhs. */
	      head_node = QO_ENV_NODE (env, bitset_first_member (&lhs_nodes));
	      tail_node = QO_ENV_NODE (env, bitset_first_member (&rhs_nodes));
	    }

	  /* Now make sure that the head and tail segs correspond to the
	     proper nodes. */
	  if (QO_SEG_HEAD (head_seg) != head_node)
	    {
	      QO_SEGMENT *tmp;

	      tmp = head_seg;
	      head_seg = tail_seg;
	      tail_seg = tmp;
	      QO_ASSERT (env, QO_SEG_HEAD (head_seg) == head_node);
	      QO_ASSERT (env, QO_SEG_HEAD (tail_seg) == tail_node);
	    }

	  /* These are really only interesting for path terms, but it doesn't
	     hurt to set them for others too. */
	  QO_TERM_SEG (term) = head_seg;
	  QO_TERM_OID_SEG (term) = tail_seg;

	  /* The term might be a merge term (i.e., it uses '=' as the
	     operator), but the expressions might not be simple attribute
	     references, and we mustn't try to establish equivalence classes
	     in that case. */
	  if (expr_is_equi_join (pt_expr))
	    {
	      qo_equivalence (head_seg, tail_seg);
	      QO_TERM_NOMINAL_SEG (term) = head_seg;
	    }

	  /* always true transitive equi-join term is not suitable as
	   * m-join edge.
	   */
	  if (PT_EXPR_INFO_IS_FLAGED (pt_expr, PT_EXPR_INFO_TRANSITIVE))
	    {
	      merge_applies = 0;
	    }
	}			/* if (merge_applies) */

      if (merge_applies)
	{
	  QO_TERM_SET_FLAG (term, QO_TERM_MERGEABLE_EDGE);
	}

      /* Now make sure that the two (node) ends of the join get cached in the
         term structure. */
      QO_TERM_HEAD (term) = head_node;
      QO_TERM_TAIL (term) = tail_node;

    }

  /* re-classify TC_JOIN term for outer join and determine its join type */
  if (QO_TERM_CLASS (term) == QO_TC_JOIN)
    {
      /* head and tail QO_NODE to which this join term belongs;
         always 'head_node' precedents to 'tail_node' and
         tail has outer join spec */
      head_node = QO_TERM_HEAD (term);
      tail_node = QO_TERM_TAIL (term);

      /* inner join until proven otherwise */
      QO_TERM_JOIN_TYPE (term) = JOIN_INNER;

      if (location > 0)
	{
	  if (QO_NODE_IDX (tail_node) > 0)
	    {
	      QO_NODE *p_node;

	      p_node = QO_ENV_NODE (env, QO_NODE_IDX (tail_node) - 1);

	      /* if explicit inner join */
	      if (QO_NODE_PT_JOIN_TYPE (tail_node) == PT_JOIN_INNER)
		{
		  QO_TERM_JOIN_TYPE (term) = JOIN_INNER;
		  /* record explicit inner join dependecy */
		  bitset_union (&(QO_NODE_OUTER_DEP_SET (tail_node)),
				&(QO_NODE_OUTER_DEP_SET (p_node)));
		  bitset_add (&(QO_NODE_OUTER_DEP_SET (tail_node)),
			      QO_NODE_IDX (p_node));
		}
	      /* if left outer join;
	         case no.7 of term class in 'outer join TM document' */
	      if (QO_NODE_PT_JOIN_TYPE (tail_node) == PT_JOIN_LEFT_OUTER)
		{
		  QO_TERM_JOIN_TYPE (term) = JOIN_LEFT;
		  /* record outer join dependecy */
		  bitset_union (&(QO_NODE_OUTER_DEP_SET (tail_node)),
				&(QO_NODE_OUTER_DEP_SET (p_node)));
		  bitset_add (&(QO_NODE_OUTER_DEP_SET (tail_node)),
			      QO_NODE_IDX (p_node));
		}
	      /* if right outer join;
	         case no.8 of term class in 'outer join TM document' */
	      if (QO_NODE_PT_JOIN_TYPE (tail_node) == PT_JOIN_RIGHT_OUTER)
		{
		  QO_TERM_JOIN_TYPE (term) = JOIN_RIGHT;
		  /* record outer join dependecy */
		  bitset_union (&(QO_NODE_OUTER_DEP_SET (tail_node)),
				&(QO_NODE_OUTER_DEP_SET (p_node)));
		  bitset_add (&(QO_NODE_OUTER_DEP_SET (tail_node)),
			      QO_NODE_IDX (p_node));
		}
	    }

	  /* check for during join term
	   * for example:
	   *   SELECT ...
	   *   FROM X left outer join Y on ... left outer join Z on X.i = Y.i
	   */
	  if (IS_OUTER_JOIN_TYPE (QO_TERM_JOIN_TYPE (term))
	      && (QO_TERM_LOCATION (term) > QO_NODE_LOCATION (tail_node)))
	    {
	      QO_TERM_CLASS (term) = QO_TC_DURING_JOIN;
	    }
	}
      else
	{
	  int join_idx, node_idx;

	  /* if explicit join;
	     case no.9 of term class in 'outer join TM document' */

	  /* set the start node of outer join - init */
	  join_idx = -1;

	  node_idx = QO_NODE_IDX (head_node);
	  /* if the sarg term belongs to null padding table; */
	  if (QO_NODE_PT_JOIN_TYPE (tail_node) == PT_JOIN_LEFT_OUTER)
	    {
	      join_idx = node_idx;	/* case 4.2 */
	    }
	  else
	    {
	      /* NEED MORE OPTIMIZATION for furture */
	      node_idx =
		MIN (QO_NODE_IDX (head_node), QO_NODE_IDX (tail_node));
	      for (; node_idx < env->nnodes; node_idx++)
		{
		  if (QO_NODE_PT_JOIN_TYPE (QO_ENV_NODE (env, node_idx)) ==
		      PT_JOIN_RIGHT_OUTER)
		    {
		      join_idx = node_idx;	/* case 4.3 */
		      break;
		    }
		}
	    }

	  /* check for the next right outer join;
	     case no.9 of term class in 'outer join TM document' */
	  if (join_idx != -1)
	    {
	      QO_TERM_CLASS (term) = QO_TC_AFTER_JOIN;
	      QO_TERM_JOIN_TYPE (term) = NO_JOIN;

	      /* keep out from m-join edge */
	      QO_TERM_CLEAR_FLAG (term, QO_TERM_MERGEABLE_EDGE);
	    }
	}			/* if (location > 0) */
    }				/* if (QO_TERM_CLASS(term) == QO_TC_JOIN) */

wrapup:

  /* A negative selectivity means that the cardinality of the result depends
     only on the cardinality of the head, not on the product of the
     cardinalities of the head and the tail as in the usual case. */
  switch (term_type)
    {
    case PT_PATH_INNER:
      QO_TERM_JOIN_TYPE (term) = JOIN_INNER;
      if (QO_NODE_NCARD (QO_TERM_TAIL (term)) == 0)
	{
	  QO_TERM_SELECTIVITY (term) = 0.0;
	}
      else
	{
	  QO_TERM_SELECTIVITY (term) =
	    1.0 / QO_NODE_NCARD (QO_TERM_TAIL (term));
	}
      break;

    case PT_PATH_OUTER:
      {
	int t;
	QO_TERM *t_term;
	QO_NODE *t_node;

	/* Traverse previously generated terms */
	for (t = 0; t < env->nterms - 1; t++)
	  {

	    t_term = QO_ENV_TERM (env, t);

	    if (QO_TERM_CLASS (t_term) == QO_TC_PATH
		&& QO_TERM_JOIN_TYPE (t_term) == JOIN_LEFT)
	      {
		if ((QO_NODE_IDX (QO_TERM_HEAD (t_term)) ==
		     QO_NODE_IDX (head_node))
		    || (QO_NODE_IDX (QO_TERM_TAIL (t_term)) ==
			QO_NODE_IDX (head_node)))
		  {
		    /* found previously generated head_nodes's path-term
		     */

		    /* get tail node */
		    t_node = QO_TERM_TAIL (t_term);

		    /* apply ordered dependency to the tail node */
		    bitset_union (&(QO_NODE_OUTER_DEP_SET (tail_node)),
				  &(QO_NODE_OUTER_DEP_SET (t_node)));
		    bitset_add (&(QO_NODE_OUTER_DEP_SET (tail_node)),
				QO_NODE_IDX (t_node));
		  }
	      }
	  }
      }
      /* FALL THROUGH */
    case PT_PATH_OUTER_WEASEL:
      /* These can't be implemented with index scans regardless because an
         index scan won't properly implement the left-outer semantics of the
         path... */
      QO_TERM_JOIN_TYPE (term) = JOIN_LEFT;
      QO_TERM_SELECTIVITY (term) = -1.0;
      QO_TERM_CAN_USE_INDEX (term) = 0;
      break;

    case PREDICATE_TERM:
      QO_TERM_SELECTIVITY (term) = qo_expr_selectivity (env, pt_expr);
      break;

    default:
      QO_TERM_JOIN_TYPE (term) = JOIN_INNER;
      QO_TERM_SELECTIVITY (term) = -1.0;
      break;
    }				/* switch (term_type) */

  /* set flag
   * TEMPORARY CODE (DO NOT REMOVE ME) */
  if (pt_expr->or_next == NULL)
    {
      QO_TERM_SET_FLAG (term, QO_TERM_SINGLE_PRED);
    }

  bitset_delset (&lhs_segs);
  bitset_delset (&rhs_segs);
  bitset_delset (&lhs_nodes);
  bitset_delset (&rhs_nodes);
}

/*
 * qo_expr_segs () -  Returns a bitset encoding all of the join graph segments
 *		      used in the pt_expr
 *   return: BITSET
 *   env(in):
 *   pt_expr(in): pointer to a conjunct
 *   result(out): BITSET of join segments (OUTPUT PARAMETER)
 */
void
qo_expr_segs (QO_ENV * env, PT_NODE * pt_expr, BITSET * result)
{
  PT_NODE *next;

  /* remember the next link and then break it */
  next = pt_expr->next;
  pt_expr->next = NULL;

  /* use env to get the bitset to the walk functions */
  QO_ENV_TMP_BITSET (env) = result;

  (void) parser_walk_tree (env->parser, pt_expr, set_seg_expr, &env,
			   pt_continue_walk, NULL);

  /* recover the next link */
  pt_expr->next = next;

  /* reset the temp pointer so we don't have a dangler */
  QO_ENV_TMP_BITSET (env) = NULL;
}

/*
 * set_seg_expr () -
 *   return: PT_NODE *
 *   parser(in): parser environment
 *   tree(in): tree to walk
 *   arg(in):
 *   continue_walk(in):
 *
 * Note: This walk "pre" function will set a bit in the bitset for
 *      each segment associated with the PT_NAME node.
 */
static PT_NODE *
set_seg_expr (PARSER_CONTEXT * parser, PT_NODE * tree, void *arg,
	      int *continue_walk)
{
  QO_ENV *env = (QO_ENV *) * (long *) arg;

  *continue_walk = PT_CONTINUE_WALK;

  /*
   * Make sure we check all subqueries for embedded references.  This
   * stuff really ought to all be done in one pass.
   */
  switch (tree->node_type)
    {
    case PT_SPEC:
      (void) parser_walk_tree (parser, tree->info.spec.derived_table,
			       set_seg_expr, arg, pt_continue_walk, NULL);
      *continue_walk = PT_LIST_WALK;
      break;

    case PT_NAME:
      (void) set_seg_node (tree, env, QO_ENV_TMP_BITSET (env));
      *continue_walk = PT_LIST_WALK;
      break;

    case PT_DOT_:
      (void) set_seg_node (tree->info.dot.arg2, env, QO_ENV_TMP_BITSET (env));
      *continue_walk = PT_LIST_WALK;
      break;

    default:
      break;
    }

  return tree;			/* don't alter tree structure */
}

/*
 * set_seg_node () -
 *   return: nothing
 *   attr(in): attribute to set the seg for
 *   env(in): optimizer environment
 *   bitset(in): bitset in which to set the bit for the segment
 */
static void
set_seg_node (PT_NODE * attr, QO_ENV * env, BITSET * bitset)
{
  QO_NODE *node;
  QO_SEGMENT *seg;
  PT_NODE *entity;

  QO_ASSERT (env, attr->node_type == PT_NAME);

  node = lookup_node (attr, env, &entity);

  /* node will be null if this attr resolves to an enclosing scope */
  if (node != NULL && (seg = lookup_seg (node, attr, env)) != NULL)
    {
      /*
       * lookup_seg() really shouldn't ever fail here, but it used to
       * for shared variables, and it doesn't really hurt anyone just
       * to ignore failures here.
       */
      bitset_add (bitset, QO_SEG_IDX (seg));
    }

}

/*
 * expr_is_mergable () - Test if the pt_expr is an equi-join conjunct
 *   return: bool
 *   pt_expr(in):
 */
static bool
expr_is_mergable (PT_NODE * pt_expr)
{
  if (pt_expr->or_next == NULL)
    {				/* keep out OR conjunct */
      if (!pt_is_query (pt_expr->info.expr.arg1)
	  && !pt_is_query (pt_expr->info.expr.arg2))
	{
	  if (pt_expr->info.expr.op == PT_EQ)
	    {
	      return true;
	    }
	  else if (pt_expr->info.expr.op == PT_RANGE)
	    {
	      PT_NODE *between_and;

	      between_and = pt_expr->info.expr.arg2;
	      if (between_and->or_next == NULL	/* has only one range */
		  && between_and->info.expr.op == PT_BETWEEN_EQ_NA)
		{
		  return true;
		}
	    }
	}
    }

  return false;
}

/*
 * expr_is_equi_join () - Test if the pt_expr is an equi-join conjunct whose
 *			  left and right sides are simple attribute references
 *   return: bool
 *   pt_expr(in):
 */
static bool
expr_is_equi_join (PT_NODE * pt_expr)
{
  if (pt_expr->or_next == NULL)
    {				/* keep out OR conjunct */
      if (pt_is_attr (pt_expr->info.expr.arg1))
	{
	  if (pt_expr->info.expr.op == PT_EQ)
	    {
	      return pt_is_attr (pt_expr->info.expr.arg2);
	    }
	  else if (pt_expr->info.expr.op == PT_RANGE)
	    {
	      PT_NODE *between_and;

	      between_and = pt_expr->info.expr.arg2;
	      if (between_and->or_next == NULL &&	/* has only one range */
		  between_and->info.expr.op == PT_BETWEEN_EQ_NA)
		{
		  return pt_is_attr (between_and->info.expr.arg1);
		}
	    }			/* else if */
	}
    }

  return false;
}

/*
 * is_dependent_table () - Returns true iff the tree represents a dependent
 *			   derived table for this query
 *   return: bool
 *   entity(in): entity spec for a from list entry
 */
static bool
is_dependent_table (PT_NODE * entity)
{
  if (entity->info.spec.derived_table)
    {
      /* this test is too pessimistic.  The argument must depend
       * on a previous entity spec in the from list.
       * >>>> fix me some day <<<<
       */
      if (entity->info.spec.derived_table_type == PT_IS_SET_EXPR ||	/* is cselect derived table of method */
	  entity->info.spec.derived_table_type == PT_IS_CSELECT
	  || entity->info.spec.derived_table->info.query.correlation_level ==
	  1)
	{
	  return true;
	}
    }

  return false;
}

/*
 * get_term_subqueries () - walks the expression to see whether it contains any
 *			    correlated subqueries.  If so, it records the
 *			    identity of the containing term in the subquery
 *			    structure
 *   return:
 *   env(in): optimizer environment
 *   term(in):
 */
static void
get_term_subqueries (QO_ENV * env, QO_TERM * term)
{
  PT_NODE *pt_expr, *next;
  WALK_INFO info;

  if (QO_IS_FAKE_TERM (term))
    {
      /*
       * This is a pseudo-term introduced to keep track of derived
       * table dependencies.  If the dependent derived table is based
       * on a subquery, we need to find that subquery and record it in
       * the pseudo-term.
       */
      pt_expr = QO_NODE_ENTITY_SPEC (QO_TERM_TAIL (term));
    }
  else
    {
      /*
       * This is a normal term, and we need to find all of the
       * correlated subqueries contained within it.
       */
      pt_expr = QO_TERM_PT_EXPR (term);
    }

  /*
   * This should only happen for dependent derived tables, either those
   * based on a set or when checking out QO_TC_DEP_JOIN terms
   * introduced for ddt's that depend on more than one thing.
   *
   */
  if (pt_expr == NULL)
    {
      return;
    }

  next = pt_expr->next;
  pt_expr->next = NULL;

  info.env = env;
  info.term = term;

  (void) parser_walk_tree (QO_ENV_PARSER (env), pt_expr, check_subquery_pre,
			   &info, pt_continue_walk, NULL);

  pt_expr->next = next;

}

/*
 * get_opcode_rank () -
 *   return:
 *   opcode(in):
 */
static int
get_opcode_rank (PT_OP_TYPE opcode)
{
  switch (opcode)
    {
      /* Group 1 -- light */
    case PT_AND:
    case PT_OR:
    case PT_NOT:
    case PT_ASSIGN:

    case PT_IS_IN:
    case PT_IS_NOT_IN:
    case PT_BETWEEN:
    case PT_NOT_BETWEEN:

    case PT_EQ:
    case PT_EQ_SOME:
    case PT_NE_SOME:
    case PT_GE_SOME:
    case PT_GT_SOME:
    case PT_LT_SOME:
    case PT_LE_SOME:
    case PT_EQ_ALL:
    case PT_NE_ALL:
    case PT_GE_ALL:
    case PT_GT_ALL:
    case PT_LT_ALL:
    case PT_LE_ALL:

    case PT_NE:
    case PT_GE:
    case PT_GT:
    case PT_LT:
    case PT_LE:

    case PT_GT_INF:
    case PT_LT_INF:

    case PT_PLUS:
    case PT_MINUS:
    case PT_TIMES:
    case PT_DIVIDE:
    case PT_UNARY_MINUS:

    case PT_EXISTS:

    case PT_BETWEEN_AND:
    case PT_BETWEEN_GE_LE:
    case PT_BETWEEN_GE_LT:
    case PT_BETWEEN_GT_LE:
    case PT_BETWEEN_GT_LT:

    case PT_BETWEEN_EQ_NA:
    case PT_BETWEEN_INF_LE:
    case PT_BETWEEN_INF_LT:
    case PT_BETWEEN_GE_INF:
    case PT_BETWEEN_GT_INF:

    case PT_RANGE:

    case PT_SYS_DATE:
    case PT_SYS_TIME:
    case PT_SYS_TIMESTAMP:
    case PT_SYS_DATETIME:

    case PT_CURRENT_USER:
    case PT_LOCAL_TRANSACTION_ID:
    case PT_CURRENT_VALUE:
    case PT_NEXT_VALUE:

    case PT_INST_NUM:
    case PT_ROWNUM:
    case PT_ORDERBY_NUM:

    case PT_MODULUS:
    case PT_RAND:
    case PT_DRAND:
    case PT_RANDOM:
    case PT_DRANDOM:

    case PT_FLOOR:
    case PT_CEIL:
    case PT_SIGN:
    case PT_POWER:
    case PT_ROUND:
    case PT_LOG:
    case PT_EXP:
    case PT_SQRT:
    case PT_ABS:
    case PT_CHR:

    case PT_IS_NULL:
    case PT_IS_NOT_NULL:
      return RANK_EXPR_LIGHT;

      /* Group 2 -- medium */
    case PT_SETEQ:
    case PT_SETNEQ:
    case PT_SUPERSETEQ:
    case PT_SUPERSET:
    case PT_SUBSET:
    case PT_SUBSETEQ:

    case PT_POSITION:
    case PT_SUBSTRING:
    case PT_OCTET_LENGTH:
    case PT_BIT_LENGTH:

    case PT_CHAR_LENGTH:
    case PT_LOWER:
    case PT_UPPER:
    case PT_TRIM:

    case PT_LTRIM:
    case PT_RTRIM:
    case PT_LPAD:
    case PT_RPAD:
    case PT_REPLACE:
    case PT_TRANSLATE:

    case PT_STRCAT:
    case PT_TO_CHAR:
    case PT_TO_DATE:
    case PT_TO_NUMBER:
    case PT_TO_TIME:
    case PT_TO_TIMESTAMP:
    case PT_TO_DATETIME:

    case PT_TRUNC:
    case PT_INSTR:
    case PT_LEAST:
    case PT_GREATEST:
    case PT_ADD_MONTHS:
    case PT_LAST_DAY:
    case PT_MONTHS_BETWEEN:

    case PT_CASE:
    case PT_NULLIF:
    case PT_COALESCE:
    case PT_NVL:
    case PT_NVL2:
    case PT_DECODE:

    case PT_EXTRACT:
    case PT_LIKE_ESCAPE:
    case PT_CAST:

    case PT_PATH_EXPR_SET:
      return RANK_EXPR_MEDIUM;

      /* Group 3 -- heavy */
    case PT_LIKE:
    case PT_NOT_LIKE:

    case PT_ENCRYPT:
    case PT_DECRYPT:
      return RANK_EXPR_HEAVY;

    default:
      return RANK_EXPR_MEDIUM;
    }
}

/*
 * get_operand_rank () -
 *   return:
 *   node(in):
 */
static int
get_operand_rank (PT_NODE * node)
{
  int rank = RANK_DEFAULT;

  if (node)
    {
      switch (node->node_type)
	{
	case PT_NAME:
	  rank = RANK_NAME;
	  break;

	case PT_VALUE:
	  rank = RANK_VALUE;
	  break;

	case PT_EXPR:
	  rank = get_opcode_rank (node->info.expr.op);
	  break;

	case PT_FUNCTION:
	  rank = RANK_EXPR_FUNCTION;
	  break;

	default:
	  break;
	}
    }

  return rank;
}

/*
 * get_term_rank () - walks the expression to see whether it contains any
 *		      rankable things. If so, it records the rank of the
 *		      containing term
 *   return:
 *   env(in): optimizer environment
 *   term(in): term to get
 */
static void
get_term_rank (QO_ENV * env, QO_TERM * term)
{
  PT_NODE *pt_expr;

  QO_TERM_RANK (term) =
    bitset_cardinality (&(QO_TERM_SUBQUERIES (term))) * RANK_QUERY;

  if (QO_IS_FAKE_TERM (term))
    {
      pt_expr = NULL;		/* do nothing */
    }
  else
    {
      pt_expr = QO_TERM_PT_EXPR (term);
    }

  if (pt_expr == NULL)
    {
      return;
    }

  /* At here, do not traverse OR list */
  switch (pt_expr->node_type)
    {
    case PT_EXPR:
      QO_TERM_RANK (term) += get_opcode_rank (pt_expr->info.expr.op);
      if (pt_expr->info.expr.arg1)
	QO_TERM_RANK (term) += get_operand_rank (pt_expr->info.expr.arg1);
      if (pt_expr->info.expr.arg2)
	QO_TERM_RANK (term) += get_operand_rank (pt_expr->info.expr.arg2);
      if (pt_expr->info.expr.arg3)
	QO_TERM_RANK (term) += get_operand_rank (pt_expr->info.expr.arg3);
      break;

    default:
      break;
    }
}

/*
 * check_subquery_pre () - Pre routine to add to some bitset all correlated
 *			   subqueries found in an expression
 *   return: PT_NODE *
 *   parser(in): parser environmnet
 *   node(in): node to check
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
check_subquery_pre (PARSER_CONTEXT * parser, PT_NODE * node, void *arg,
		    int *continue_walk)
{
  WALK_INFO *info = (WALK_INFO *) arg;

  /*
   * Be sure to reenable walking for list tails.
   */
  *continue_walk = PT_CONTINUE_WALK;

  if (node->node_type == PT_SELECT
      || node->node_type == PT_UNION
      || node->node_type == PT_DIFFERENCE
      || node->node_type == PT_INTERSECTION)
    {
      *continue_walk = PT_LIST_WALK;	/* NEVER need to look inside queries */

      if (node->info.query.correlation_level == 1)
	{
	  /*
	   * Find out the index of this subquery, and record that index
	   * in the enclosing term's subquery bitset.  This is lame,
	   * but I can't think of a better way to do it.  When we
	   * originally grabbed all of the subqueries we had no idea
	   * what expression they were in, so we have to discover it
	   * after the fact.  Oh well, this doesn't happen often
	   * anyway.
	   */
	  int i, N;
	  QO_ENV *env;

	  env = info->env;
	  for (i = 0, N = env->nsubqueries; i < N; i++)
	    {
	      if (node == env->subqueries[i].node)
		{
		  bitset_add (&(env->subqueries[i].terms),
			      QO_TERM_IDX (info->term));
		  bitset_add (&(QO_TERM_SUBQUERIES (info->term)), i);
		  break;
		}
	    }
	}
    }

  return node;			/* leave node unchanged */

}

/*
 * is_local_name () -
 *   return: 1 iff the expression is a name correlated to the current query
 *   env(in): Optimizer environment
 *   expr(in): The parse tree for the expression to examine
 */
static bool
is_local_name (QO_ENV * env, PT_NODE * expr)
{
  UINTPTR spec = 0;

  if (expr == NULL)
    {
      return false;
    }
  else if (expr->node_type == PT_NAME)
    {
      spec = expr->info.name.spec_id;
    }
  else if (expr->node_type == PT_DOT_)
    {
      spec = expr->info.dot.arg2->info.name.spec_id;
    }
  else
    {
      return false;
    }

  return (pt_find_entity (env->parser,
			  env->pt_tree->info.query.q.select.from,
			  spec) != NULL) ? true : false;
}

/*
 * is_pseudo_const () -
 *   return: 1 iff the expression can server as a pseudo-constant
 *	     during predicate evaluation.  Used primarily to help
 *	     determine whether a predicate can be implemented
 *	     with an index scan
 *   env(in): The optimizer environment
 *   expr(in): The parse tree for the expression to examine
 */
static bool
is_pseudo_const (QO_ENV * env, PT_NODE * expr)
{
  if (expr == NULL)
    {
      return false;
    }

  switch (expr->node_type)
    {
    case PT_VALUE:
    case PT_HOST_VAR:
      return true;

    case PT_SELECT:
    case PT_UNION:
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
      return (expr->info.query.correlation_level != 1) ? true : false;

    case PT_NAME:
      /*
       * It is up to the calling context to ensure that the name is
       * actually a pseudo constant, either because it is a correlated
       * outer reference, or because it can otherwise be guaranteed to
       * be evaluated by the time it is referenced.
       */
      return true;

    case PT_DOT_:
      /*
       * It would be nice if we could use expressions that are
       * guaranteed to be independent of the attribute, but the current
       * XASL implementation can't guarantee that such expressions have
       * been evaluated by the time that we need them, so we have to
       * play it safe here and not use them.
       */
      return true;

    case PT_EXPR:
      switch (expr->info.expr.op)
	{
	case PT_PLUS:
	case PT_STRCAT:
	case PT_MINUS:
	case PT_TIMES:
	case PT_DIVIDE:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env,
				      expr->info.expr.arg2)) ? true : false;
	case PT_UNARY_MINUS:
	  return is_pseudo_const (env, expr->info.expr.arg1);
	case PT_BETWEEN_AND:
	case PT_BETWEEN_GE_LE:
	case PT_BETWEEN_GE_LT:
	case PT_BETWEEN_GT_LE:
	case PT_BETWEEN_GT_LT:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env,
				      expr->info.expr.arg2)) ? true : false;
	case PT_BETWEEN_EQ_NA:
	case PT_BETWEEN_INF_LE:
	case PT_BETWEEN_INF_LT:
	case PT_BETWEEN_GE_INF:
	case PT_BETWEEN_GT_INF:
	  return is_pseudo_const (env, expr->info.expr.arg1);
	case PT_MODULUS:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env,
				      expr->info.expr.arg2)) ? true : false;
	case PT_RAND:
	case PT_DRAND:
	  return true;
	case PT_FLOOR:
	case PT_CEIL:
	case PT_SIGN:
	case PT_ABS:
	case PT_CHR:
	case PT_EXP:
	case PT_SQRT:
	  return is_pseudo_const (env, expr->info.expr.arg1);
	case PT_POWER:
	case PT_ROUND:
	case PT_TRUNC:
	case PT_LOG:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env,
				      expr->info.expr.arg2)) ? true : false;
	case PT_INSTR:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env, expr->info.expr.arg2)
		  && is_pseudo_const (env,
				      expr->info.expr.arg3)) ? true : false;
	case PT_POSITION:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env,
				      expr->info.expr.arg2)) ? true : false;
	case PT_SUBSTRING:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env, expr->info.expr.arg2)
		  && (expr->info.expr.arg3 ?
		      is_pseudo_const (env,
				       expr->info.expr.
				       arg3) : true)) ? true : false;
	case PT_CHAR_LENGTH:
	case PT_OCTET_LENGTH:
	case PT_BIT_LENGTH:
	case PT_LOWER:
	case PT_UPPER:
	  return is_pseudo_const (env, expr->info.expr.arg1);
	case PT_TRIM:
	case PT_LTRIM:
	case PT_RTRIM:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && (expr->info.expr.arg2 ?
		      is_pseudo_const (env,
				       expr->info.expr.
				       arg2) : true)) ? true : false;

	case PT_LPAD:
	case PT_RPAD:
	case PT_REPLACE:
	case PT_TRANSLATE:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env, expr->info.expr.arg2)
		  && (expr->info.expr.arg3 ?
		      is_pseudo_const (env,
				       expr->info.expr.
				       arg3) : true)) ? true : false;
	case PT_ADD_MONTHS:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env,
				      expr->info.expr.arg2)) ? true : false;
	case PT_LAST_DAY:
	  return is_pseudo_const (env, expr->info.expr.arg1);
	case PT_MONTHS_BETWEEN:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env,
				      expr->info.expr.arg2)) ? true : false;
	case PT_SYS_DATE:
	case PT_SYS_TIME:
	case PT_SYS_TIMESTAMP:
	case PT_SYS_DATETIME:
	case PT_LOCAL_TRANSACTION_ID:
	case PT_CURRENT_USER:
	  return true;
	case PT_TO_CHAR:
	case PT_TO_DATE:
	case PT_TO_TIME:
	case PT_TO_TIMESTAMP:
	case PT_TO_DATETIME:
	case PT_TO_NUMBER:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && (expr->info.expr.arg2 ?
		      is_pseudo_const (env,
				       expr->info.expr.
				       arg2) : true)) ? true : false;
	case PT_CURRENT_VALUE:
	case PT_NEXT_VALUE:
	  return true;
	case PT_CAST:
	  return is_pseudo_const (env, expr->info.expr.arg1);
	case PT_CASE:
	case PT_DECODE:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env,
				      expr->info.expr.arg2)) ? true : false;
	case PT_NULLIF:
	case PT_COALESCE:
	case PT_NVL:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env,
				      expr->info.expr.arg2)) ? true : false;
	case PT_NVL2:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env, expr->info.expr.arg2)
		  && is_pseudo_const (env,
				      expr->info.expr.arg3)) ? true : false;
	case PT_EXTRACT:
	  return is_pseudo_const (env, expr->info.expr.arg1);
	case PT_LEAST:
	case PT_GREATEST:
	  return (is_pseudo_const (env, expr->info.expr.arg1)
		  && is_pseudo_const (env,
				      expr->info.expr.arg2)) ? true : false;
	default:
	  return false;
	}

    case PT_FUNCTION:
      {
	/*
	 * The is the case we encounter for predicates like
	 *
	 *      x in (a,b,c)
	 *
	 * Here the the expression '(a,b,c)' comes in as a multiset
	 * function call, with PT_NAMEs 'a', 'b', and 'c' as its arglist.
	 */
	PT_NODE *p;

	if (expr->info.function.function_type != F_SET
	    && expr->info.function.function_type != F_MULTISET
	    && expr->info.function.function_type != F_SEQUENCE)
	  {
	    return false;
	  }
	for (p = expr->info.function.arg_list; p; p = p->next)
	  {
	    if (!is_pseudo_const (env, p))
	      {
		return false;
	      }
	  }
	return true;
      }

    default:
      return false;
    }
}

/*
 * add_local_subquery () - This routine adds an entry to the optimizer
 *			   environment for the subquery
 *   return: nothing
 *   env(in): Optimizer environment
 *   node(in): The parse tree for the subquery being added
 */
static void
add_local_subquery (QO_ENV * env, PT_NODE * node)
{
  int i, n;
  QO_SUBQUERY *tmp;

  n = env->nsubqueries++;

  /*
   * Be careful here: the previously allocated QO_SUBQUERY terms
   * contain bitsets that may have self-relative internal pointers, and
   * those pointers have to be maintained in the new array.  The proper
   * way to make sure that they are consistent is to use the bitset_assign()
   * macro, not just to do the bitcopy that memcpy() will do.
   */
  tmp = NALLOCATE (env, QO_SUBQUERY, n + 1);
  memcpy (tmp, env->subqueries, n * sizeof (QO_SUBQUERY));
  for (i = 0; i < n; i++)
    {
      QO_SUBQUERY *subq;
      subq = &env->subqueries[i];
      BITSET_MOVE (tmp[i].segs, subq->segs);
      BITSET_MOVE (tmp[i].nodes, subq->nodes);
      BITSET_MOVE (tmp[i].terms, subq->terms);
    }
  DEALLOCATE (env, env->subqueries);
  env->subqueries = tmp;

  tmp = &env->subqueries[n];
  tmp->node = node;
  bitset_init (&tmp->segs, env);
  bitset_init (&tmp->nodes, env);
  bitset_init (&tmp->terms, env);
  qo_expr_segs (env, node, &tmp->segs);
  qo_seg_nodes (env, &tmp->segs, &tmp->nodes);
  tmp->idx = n;
}


/*
 * get_local_subqueries_pre () - Builds vector of locally correlated
 *				 (level 1) queries
 *   return:
 *   parser(in):
 *   node(in):
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
get_local_subqueries_pre (PARSER_CONTEXT * parser, PT_NODE * node, void *arg,
			  int *continue_walk)
{
  QO_ENV *env;
  BITSET segs;

  *continue_walk = PT_CONTINUE_WALK;

  switch (node->node_type)
    {
    case PT_SELECT:
    case PT_UNION:
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
      /* check for correlated subquery except for SELECT list */
      if (node->info.query.correlation_level == 1)
	{
	  env = (QO_ENV *) arg;
	  bitset_init (&segs, env);
	  qo_expr_segs (env, node, &segs);
	  if (bitset_is_empty (&segs))
	    {
	      /* reduce_equality_terms() can change a correlated subquery to
	       * uncorrelated one */
	      node->info.query.correlation_level = 0;
	    }
	  bitset_delset (&segs);
	}
      *continue_walk = PT_LIST_WALK;
      break;

    default:
      break;
    }

  return node;
}

/*
 * get_local_subqueries_post () - Builds vector of locally correlated
 *				  (level 1) queries
 *   return:
 *   parser(in):
 *   node(in):
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
get_local_subqueries_post (PARSER_CONTEXT * parser, PT_NODE * node, void *arg,
			   int *continue_walk)
{
  QO_ENV *env = (QO_ENV *) arg;

  *continue_walk = PT_CONTINUE_WALK;

  switch (node->node_type)
    {
    case PT_SELECT:
    case PT_UNION:
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
      if (node->info.query.correlation_level == 1)
	{
	  add_local_subquery (env, node);
	}
      break;

    default:
      break;
    }

  return node;
}

/*
 * get_local_subqueries () -
 *   return: non-zero if something went wrong
 *   env(in):
 *   node(in):
 *
 * Note:
 *	Gather the correlated level == 1 subqueries.
 *	EXCLUDE nested queries.
 *	INCLUDING the node being passed in.
 */
static void
get_local_subqueries (QO_ENV * env, PT_NODE * node)
{
  PARSER_CONTEXT *parser;
  PT_NODE *tree;
  PT_NODE *select_list_ptr;
  PT_NODE *next_ptr;
  int i;

  parser = QO_ENV_PARSER (env);
  tree = QO_ENV_PT_TREE (env);

  next_ptr = tree->next;
  tree->next = NULL;
  select_list_ptr = tree->info.query.q.select.list;
  tree->info.query.q.select.list = NULL;

  parser_walk_leaves (parser,
		      tree,
		      get_local_subqueries_pre, env,
		      get_local_subqueries_post, env);

  /* restore next list pointer */
  tree->next = next_ptr;
  tree->info.query.q.select.list = select_list_ptr;

  /*
   * Now that all of the subqueries have been discovered, make
   * *another* pass and associate each with its enclosing QO_TERM, if
   * any.
   */
  for (i = 0; i < env->nterms; i++)
    {
      get_term_subqueries (env, QO_ENV_TERM (env, i));
    }

  QO_ASSERT (env, env->subqueries != NULL || env->nsubqueries == 0);

}


/*
 * get_rank () - Gather term's rank
 *   return:
 *   env(in):
 */
static void
get_rank (QO_ENV * env)
{
  int i;

  for (i = 0; i < env->nterms; i++)
    {
      get_term_rank (env, QO_ENV_TERM (env, i));
    }
}

/*
 * get_referenced_attrs () - Returns the list of this entity's attributes that
 *			     are referenced in this query
 *   return:
 *   entity(in):
 */
static PT_NODE *
get_referenced_attrs (PT_NODE * entity)
{
  return (entity->info.spec.derived_table
	  ? entity->info.spec.as_attr_list
	  : entity->info.spec.referenced_attrs);
}

/*
 * add_hint_args () - attach hint informations to QO_NODEs
 *   return:
 *   env(in):
 *   arg_list(in):
 *   hint(in):
 */
static void
add_hint_args (QO_ENV * env, PT_NODE * arg_list, PT_HINT_ENUM hint)
{
  PT_NODE *arg, *entity_spec;
  QO_NODE *node;
  int i;

  if (arg_list)
    {
      /* iterate over all nodes */
      for (i = 0; i < env->nnodes; i++)
	{
	  node = QO_ENV_NODE (env, i);
	  entity_spec = QO_NODE_ENTITY_SPEC (node);
	  /* check for spec list */
	  for (arg = arg_list; arg; arg = arg->next)
	    {
	      /* found match */
	      if (entity_spec->info.spec.id == arg->info.name.spec_id)
		{
		  QO_NODE_HINT (node) =
		    (PT_HINT_ENUM) (QO_NODE_HINT (node) | hint);
		  break;
		}
	    }
	}
    }
  else
    {				/* FULLY HINTED */
      /* iterate over all nodes */
      for (i = 0; i < env->nnodes; i++)
	{
	  node = QO_ENV_NODE (env, i);
	  QO_NODE_HINT (node) = (PT_HINT_ENUM) (QO_NODE_HINT (node) | hint);
	}
    }

}

/*
 * add_hint () -
 *   return:
 *   env(in):
 *   tree(in):
 */
static void
add_hint (QO_ENV * env, PT_NODE * tree)
{
  PT_HINT_ENUM hint;
  int i, j, k;
  QO_NODE *node, *p_node;
  PT_NODE *arg, *p_arg, *spec, *p_spec;
  int last_ordered_idx = 0;

  hint = tree->info.query.q.select.hint;

  if (hint & PT_HINT_ORDERED)
    {
      if (tree->info.query.q.select.ordered)
	{
	  /* find last ordered node */
	  for (arg = tree->info.query.q.select.ordered;
	       arg->next; arg = arg->next)
	    {
	      ;			/* nop */
	    }
	  for (i = 0; i < env->nnodes; i++)
	    {
	      node = QO_ENV_NODE (env, i);
	      spec = QO_NODE_ENTITY_SPEC (node);
	      if (spec->info.spec.id == arg->info.name.spec_id)
		{
		  last_ordered_idx = QO_NODE_IDX (node);
		  break;
		}
	    }

	  /* iterate over all nodes */
	  for (i = 0; i < env->nnodes; i++)
	    {
	      node = QO_ENV_NODE (env, i);
	      spec = QO_NODE_ENTITY_SPEC (node);
	      /* check for arg list */
	      p_arg = NULL;
	      for (arg = tree->info.query.q.select.ordered, j = 0;
		   arg; arg = arg->next, j++)
		{
		  if (spec->info.spec.id == arg->info.name.spec_id)
		    {
		      if (p_arg)
			{	/* skip out the first ordered spec */
			  /* find prev node */
			  for (k = 0; k < env->nnodes; k++)
			    {
			      p_node = QO_ENV_NODE (env, k);
			      p_spec = QO_NODE_ENTITY_SPEC (p_node);
			      if (p_spec->info.spec.id ==
				  p_arg->info.name.spec_id)
				{
				  bitset_assign (&
						 (QO_NODE_OUTER_DEP_SET
						  (node)),
						 &(QO_NODE_OUTER_DEP_SET
						   (p_node)));
				  bitset_add (&(QO_NODE_OUTER_DEP_SET (node)),
					      QO_NODE_IDX (p_node));
				  break;
				}
			    }
			}

#if 1				/* TEMPORARY CODE: DO NOT REMOVE ME !!! */
		      QO_NODE_HINT (node) =
			(PT_HINT_ENUM) (QO_NODE_HINT (node) |
					PT_HINT_ORDERED);
#endif
		      break;	/* exit loop for arg traverse */
		    }

		  p_arg = arg;	/* save previous arg */
		}

	      /* not found in arg list */
	      if (!arg)
		{
		  bitset_add (&(QO_NODE_OUTER_DEP_SET (node)),
			      last_ordered_idx);
		}

	    }			/* for (i = ... ) */

	}
      else
	{			/* FULLY HINTED */
	  /* iterate over all nodes */
	  p_node = NULL;
	  for (i = 0; i < env->nnodes; i++)
	    {
	      node = QO_ENV_NODE (env, i);
	      if (p_node)
		{		/* skip out the first ordered node */
		  bitset_assign (&(QO_NODE_OUTER_DEP_SET (node)),
				 &(QO_NODE_OUTER_DEP_SET (p_node)));
		  bitset_add (&(QO_NODE_OUTER_DEP_SET (node)),
			      QO_NODE_IDX (p_node));
		}
#if 1				/* TEMPORARY CODE: DO NOT REMOVE ME !!! */
	      QO_NODE_HINT (node) =
		(PT_HINT_ENUM) (QO_NODE_HINT (node) | PT_HINT_ORDERED);
#endif

	      p_node = node;	/* save previous node */
	    }			/* for (i = ... ) */
	}
    }

  if (hint & PT_HINT_W)
    {				/* not used */
    }
  if (hint & PT_HINT_X)
    {				/* not used */
    }
  if (hint & PT_HINT_Y)
    {				/* not used */
    }

  if (hint & PT_HINT_USE_NL)
    {
      add_hint_args (env, tree->info.query.q.select.use_nl, PT_HINT_USE_NL);
    }

  if (hint & PT_HINT_USE_IDX)
    {
      add_hint_args (env, tree->info.query.q.select.use_idx, PT_HINT_USE_IDX);
    }

  if (hint & PT_HINT_USE_MERGE)
    {
      add_hint_args (env, tree->info.query.q.select.use_merge,
		     PT_HINT_USE_MERGE);
    }

  if (hint & PT_HINT_USE_HASH)
    {				/* not used */
    }

}

/*
 * add_using_index () - attach index names specified in USING INDEX clause
 *			to QO_NODEs
 *   return:
 *   env(in):
 *   using_index(in):
 */
static void
add_using_index (QO_ENV * env, PT_NODE * using_index)
{
  int i, n;
  QO_NODE *nodep;
  QO_USING_INDEX *uip;
  PT_NODE *indexp;

  if (!using_index)
    {
      /* no USING INDEX clause in the query;
         all QO_NODE_USING_INDEX(node) will contain NULL */
      return;
    }

  /* for each node */
  for (i = 0; i < env->nnodes; i++)
    {
      nodep = QO_ENV_NODE (env, i);

      /* count number of indexes for this node */
      n = 0;
      for (indexp = using_index; indexp; indexp = indexp->next)
	{
	  if (indexp->info.name.original == NULL
	      && indexp->info.name.resolved == NULL)
	    {
	      break;		/* USING INDEX NONE case */
	    }
	  if (indexp->info.name.original == NULL
	      && indexp->info.name.resolved[0] == '*')
	    {
	      n++;		/* USING INDEX ALL EXCEPT case */
	    }
	  if (indexp->info.name.original
	      && !intl_mbs_casecmp (QO_NODE_NAME (nodep),
				    indexp->info.name.resolved))
	    {
	      n++;
	    }
	}
      /* if n == 0, it means that either no indexes in USING INDEX clause for
         this node or USING INDEX NONE case */

      /* allocate QO_USING_INDEX structure */
      uip = QO_NODE_USING_INDEX (nodep) =
	(n == 0) ? ALLOC_USING_INDEX (env, 1) : ALLOC_USING_INDEX (env, n);
      QO_UI_N (uip) = n;
      /* attach indexes to QO_NODE */
      n = 0;
      for (indexp = using_index; indexp; indexp = indexp->next)
	{
	  if (indexp->info.name.original == NULL
	      && indexp->info.name.resolved == NULL)
	    {
	      break;		/* USING INDEX NONE case */
	    }
	  if (indexp->info.name.original == NULL
	      && indexp->info.name.resolved[0] == '*')
	    {
	      /* USING INDEX ALL EXCEPT case */
	      QO_UI_INDEX (uip, n) = indexp->info.name.resolved;
	      QO_UI_FORCE (uip, n++) = (int) (indexp->etc);
	    }
	  if (indexp->info.name.original
	      && !intl_mbs_casecmp (QO_NODE_NAME (nodep),
				    indexp->info.name.resolved))
	    {
	      QO_UI_INDEX (uip, n) = indexp->info.name.original;
	      QO_UI_FORCE (uip, n++) = (int) (indexp->etc);
	    }
	}
    }
}

/*
 * qo_alloc_index () - Allocate a QO_INDEX structure with room for <n>
 *		       QO_INDEX_ENTRY elements.  The fields are initialized
 *   return: QO_CLASS_INFO *
 *   env(in): The current optimizer environment
 *   n(in): The node whose class info we want
 */
static QO_INDEX *
qo_alloc_index (QO_ENV * env, int n)
{
  int i;
  QO_INDEX *indexp;
  QO_INDEX_ENTRY *entryp;

  indexp = ALLOC_INDEX (env, n);
  indexp->n = 0;
  indexp->max = n;

  for (i = 0; i < n; i++)
    {
      entryp = QO_INDEX_INDEX (indexp, i);

      entryp->next = NULL;
      entryp->type = SM_CONSTRAINT_INDEX;
      entryp->class_ = NULL;
      BTID_SET_NULL (&(entryp->btid));
      entryp->name = NULL;
      entryp->col_num = 0;
      entryp->stats = NULL;
      entryp->bt_stats_idx = -1;
      entryp->nsegs = 0;
      entryp->seg_idxs = NULL;
      entryp->seg_equal_terms = NULL;
      entryp->seg_other_terms = NULL;
      bitset_init (&(entryp->terms), env);
    }

  return indexp;
}

/*
 * qo_free_index () - Free the QO_INDEX structure and all elements contained
 *		      within it
 *   return: nothing
 *   env(in): The current optimizer environment
 *   indexp(in): A pointer to a previously-allocated index vector
 */
static void
qo_free_index (QO_ENV * env, QO_INDEX * indexp)
{
  int i, j;
  QO_INDEX_ENTRY *entryp;

  if (!indexp)
    {
      return;
    }

  for (i = 0; i < indexp->max; i++)
    {
      entryp = QO_INDEX_INDEX (indexp, i);
      bitset_delset (&(entryp->terms));
      for (j = 0; j < entryp->nsegs; j++)
	{
	  bitset_delset (&(entryp->seg_equal_terms[j]));
	  bitset_delset (&(entryp->seg_other_terms[j]));
	}
      if (entryp->nsegs)
	{
	  DEALLOCATE (env, entryp->seg_equal_terms);
	  DEALLOCATE (env, entryp->seg_other_terms);
	  DEALLOCATE (env, entryp->seg_idxs);
	}
    }

  DEALLOCATE (env, indexp);
}

/*
 * qo_get_class_info () -
 *   return: QO_CLASS_INFO *
 *   env(in): The current optimizer environment
 *   node(in): The node whose class info we want
 */
static QO_CLASS_INFO *
qo_get_class_info (QO_ENV * env, QO_NODE * node)
{
  PT_NODE *dom_set;
  int n;
  QO_CLASS_INFO *info;
  QO_CLASS_INFO_ENTRY *end;
  int i;

  dom_set = QO_NODE_ENTITY_SPEC (node)->info.spec.flat_entity_list;
  n = count_classes (dom_set);
  info = ALLOC_CLASS_INFO (env, n);

  for (i = 0; i < n; ++i)
    {
      info->info[i].name = NULL;
      info->info[i].mop = NULL;
      info->info[i].smclass = NULL;
      info->info[i].stats = NULL;
      info->info[i].self_allocated = 0;
      OID_SET_NULL (&info->info[i].oid);
      info->info[i].index = NULL;
    }

  info->n = n;
  end = grok_classes (env, dom_set, &info->info[0]);

  QO_ASSERT (env, end == &info->info[n]);

  return info;

}

/*
 * qo_free_class_info () - Free the vector and all interally-allocated
 *			   structures
 *   return: nothing
 *   env(in): The current optimizer environment
 *   info(in): A pointer to a previously-allocated info vector
 */
static void
qo_free_class_info (QO_ENV * env, QO_CLASS_INFO * info)
{
  int i;

  if (info == NULL)
    {
      return;
    }

  /*
   * The CLASS_STATS structures that are pointed to by the various
   * members of info[] will be automatically freed by the garbage
   * collector.  Make sure that we null out our mop pointer so that the
   * garbage collector doesn't mistakenly believe that the class object
   * is still in use.
   */
  for (i = 0; i < info->n; ++i)
    {
      qo_free_index (env, info->info[i].index);
      info->info[i].name = NULL;
      info->info[i].mop = NULL;
      if (info->info[i].self_allocated)
	DEALLOCATE (env, info->info[i].stats);
      info->info[i].smclass = NULL;
    }
  DEALLOCATE (env, info);

}

/*
 * count_classes () - Count the number of object-based classes in the domain set
 *   return: int
 *   p(in):
 */
static int
count_classes (PT_NODE * p)
{
  int n;

  for (n = 0; p; p = p->next)
    {
      n++;
    }

  return n;
}

/*
 * grok_classes () -
 *   return: QO_CLASS_INFO_ENTRY *
 *   env(in): The current optimizer environment
 *   p(in): The flat list of entity_specs
 *   info(in): The next info slot to be initialized
 *
 * Note: Populate the info array by traversing the given flat list.
 *	info is assumed to point to a vector of QO_CLASS_INFO_ENTRY
 *	structures that is long enough to accept entries for all
 *	remaining object-based classes.  This should be the case if
 *	the length of the array was determined using count_classes()
 *	above.
 */
static QO_CLASS_INFO_ENTRY *
grok_classes (QO_ENV * env, PT_NODE * p, QO_CLASS_INFO_ENTRY * info)
{
  HFID *hfid;
  SM_CLASS *smclass;

  for (; p; p = p->next)
    {
      info->mop = p->info.name.db_object;
      info->normal_class = db_is_class (info->mop);
      if (info->mop)
	{
	  info->oid = *WS_OID (info->mop);
	  info->name = sm_class_name (info->mop);
	  info->smclass = sm_get_class_with_statistics (info->mop);
	}
      else
	{
	  PARSER_CONTEXT *parser = env->parser;
	  PT_INTERNAL_ERROR (parser, "info");
	  return info;
	}

      smclass = info->smclass;
      if (smclass == NULL)
	{
	  PARSER_CONTEXT *parser = env->parser;
	  PT_INTERNAL_ERROR (parser, "info");
	  return info;
	}

      if (smclass->stats == NULL)
	{
	  info->stats = ALLOCATE (env, CLASS_STATS);
	  info->self_allocated = 1;
	  info->stats->n_attrs = 0;
	  info->stats->attr_stats = NULL;
	  qo_estimate_statistics (info->mop, info->stats);
	}
      else if (smclass->stats->heap_size == 0)
	{
	  /*
	   * Be careful here: if this is a proxy for some ldb, it will
	   * look like an empty class, when in reality it may be
	   * something huge.  Make sure that we use the big estimate in
	   * that case.
	   *
	   * If it's not a proxy and it seems to have a heap associated
	   * with it, assume that the statistics manager is confused.
	   */
	  if (!info->normal_class
	      || (((hfid = sm_get_heap (info->mop)) && !HFID_IS_NULL (hfid))))
	    {
	      qo_estimate_statistics (info->mop, smclass->stats);
	    }
	}

      info++;
    }

  return info;
}

/*
 * qo_get_attr_info () - Find the ATTR_STATS information about each actual
 *			 attribute that underlies this segment
 *   return: QO_ATTR_INFO *
 *   env(in): The current optimizer environment
 *   seg(in): A (pointer to) a join graph segment
 */
static QO_ATTR_INFO *
qo_get_attr_info (QO_ENV * env, QO_SEGMENT * seg)
{
  QO_NODE *nodep;
  QO_CLASS_INFO_ENTRY *class_info_entryp;
  QO_ATTR_INFO *attr_infop;
  int attr_id;
  QO_ATTR_CUM_STATS *cum_statsp;
  ATTR_STATS *attr_statsp;
  BTREE_STATS *bt_statsp;
  int n_attrs;
  const char *name;
  int n, i, j;
  bool is_unique_index;
  SM_CLASS_CONSTRAINT *constraints, *consp;

  /* actual attribute name of the given segment */
  name = QO_SEG_NAME (seg);
  /* QO_NODE of the given segment */
  nodep = QO_SEG_HEAD (seg);

  if (QO_NODE_INFO (nodep) == NULL ||
      !(QO_NODE_INFO (nodep)->info[0].normal_class))
    {
      /* if there's no class information or the class is not normal class */
      return NULL;
    }

  /* number of class information entries */
  n = QO_NODE_INFO_N (nodep);
  QO_ASSERT (env, n > 0);

  /* pointer to QO_CLASS_INFO_ENTRY[] array of the node */
  class_info_entryp = &QO_NODE_INFO (nodep)->info[0];

  /* allocate QO_ATTR_INFO within the current optimizer environment */
  attr_infop = ALLOC_ATTR_INFO (env);
  if (!attr_infop)
    {
      /* already, error has been set */
      return NULL;
    }

  /* initialize QO_ATTR_CUM_STATS structure of QO_ATTR_INFO */
  cum_statsp = &attr_infop->cum_stats;
  cum_statsp->type = sm_att_type_id (class_info_entryp->mop, name);
  cum_statsp->valid_limits = false;
  OR_PUT_INT (&cum_statsp->min_value, 0);
  OR_PUT_INT (&cum_statsp->max_value, 0);
  cum_statsp->is_indexed = true;
  cum_statsp->leafs = cum_statsp->pages = cum_statsp->height =
    cum_statsp->keys = cum_statsp->oids = cum_statsp->nulls =
    cum_statsp->ukeys = 0;
  cum_statsp->key_type = NULL;
  cum_statsp->key_size = 0;
  cum_statsp->pkeys = NULL;

  /* set the statistics from the class information(QO_CLASS_INFO_ENTRY) */
  for (i = 0; i < n; class_info_entryp++, i++)
    {

      attr_id = sm_att_id (class_info_entryp->mop, name);

      /* pointer to ATTR_STATS of CLASS_STATS of QO_CLASS_INFO_ENTRY */
      attr_statsp = QO_GET_CLASS_STATS (class_info_entryp)->attr_stats;
      if (!attr_statsp)
	{
	  /* the attribute statistics of the class were not set */
	  cum_statsp->is_indexed = false;
	  continue;
	  /* We'll consider the segment to be indexed only if all of the
	     attributes it represents are indexed. The current optimization
	     strategy makes it inconvenient to try to construct "mixed"
	     (segment and index) scans of a node that represents more than
	     one node. */
	}


      /* The stats vector isn't kept in id order because of the effects
         of schema updates (attribute deletion, most notably). We need
         to search it to find the stats record we're interested in.
         Worse, there doesn't even need to be an entry for this particular
         attribute in the vector. If we're dealing with a class that was
         created after the last statistics update, it won't have any
         information associated with it, or if we're dealing with certain
         kinds of attributes they simply won't be recorded. In these cases
         we just make the best guess we can. */

      /* search the attribute from the class information */
      n_attrs = QO_GET_CLASS_STATS (class_info_entryp)->n_attrs;
      for (j = 0; j < n_attrs; j++, attr_statsp++)
	{
	  if (attr_statsp->id == attr_id)
	    {
	      break;
	    }
	}
      if (j == n_attrs)
	{
	  /* attribute not found, what happens to the class attribute? */
	  cum_statsp->is_indexed = false;
	  continue;
	}

      /* if the atrribute is numeric type so its min/max values are
         meaningful, keep the min/max existing values */
      if (DB_NUMERIC_TYPE (attr_statsp->type))
	{

	  if (!cum_statsp->valid_limits)
	    {

	      /* first time */
	      cum_statsp->type = attr_statsp->type;
	      cum_statsp->valid_limits = true;
	      /* assign values, bitwise-copy of DB_DATA structure */
	      cum_statsp->min_value = attr_statsp->min_value;
	      cum_statsp->max_value = attr_statsp->max_value;

	    }
	  else
	    {			/* if (!cum_statsp->valid_limits) */

	      /* compare with previsous values */
	      if (qo_data_compare (&attr_statsp->min_value,
				   &cum_statsp->min_value,
				   cum_statsp->type) < 0)
		{
		  cum_statsp->min_value = attr_statsp->min_value;
		}
	      if (qo_data_compare (&attr_statsp->max_value,
				   &cum_statsp->max_value,
				   cum_statsp->type) > 0)
		{
		  cum_statsp->max_value = attr_statsp->max_value;
		}
	      /* 'qo_data_compare()' is a simplized function that works
	         with DB_DATA instead of DB_VALUE. However, this way
	         would be enough to get minimum/maximum existing value,
	         because the values are meaningful only when their types
	         are numeric and we are considering compatible indexes
	         under class hierarchy. */

	    }			/* if (!cum_statsp->valid_limits) */

	}			/* if (DB_NUMERIC_TYPE(attr_statsp->type)) */


      if (attr_statsp->n_btstats <= 0 || !attr_statsp->bt_stats)
	{
	  /* the attribute dose not have any index */
	  cum_statsp->is_indexed = false;
	  continue;
	  /* We'll consider the segment to be indexed only if all of the
	     attributes it represents are indexed. The current optimization
	     strategy makes it inconvenient to try to construct "mixed"
	     (segment and index) scans of a node that represents more than
	     one node. */
	}

      /* Because we cannot know which index will be selected for this
         attribute when there're more than one indexes on this attribute,
         use the statistics of the MIN keys index. */
      bt_statsp = &attr_statsp->bt_stats[0];
      for (j = 1; j < attr_statsp->n_btstats; j++)
	{
	  if (bt_statsp->keys > attr_statsp->bt_stats[j].keys)
	    {
	      bt_statsp = &attr_statsp->bt_stats[j];
	    }
	}

      if (QO_NODE_ENTITY_SPEC (nodep)->info.spec.only_all == PT_ALL)
	{
	  /* class hierarchy spec
	     for example: select ... from all p */

	  /* check index uniqueness */
	  is_unique_index = false;	/* init */

	  constraints = sm_class_constraints (class_info_entryp->mop);
	  for (consp = constraints; consp; consp = consp->next)
	    {
	      /* found index */
	      if (SM_IS_CONSTRAINT_INDEX_FAMILY (consp->type)
		  && BTID_IS_EQUAL (&bt_statsp->btid, &consp->index))
		{
		  if (SM_IS_CONSTRAINT_UNIQUE_FAMILY (consp->type))
		    {
		      is_unique_index = true;
		    }
		  break;
		}
	    }

	  if (is_unique_index)
	    {
	      /* is class hierarchy index: set unique index statistics */
	      cum_statsp->leafs = bt_statsp->leafs;
	      cum_statsp->pages = bt_statsp->pages;
	      cum_statsp->oids = bt_statsp->oids;
	      cum_statsp->nulls = bt_statsp->nulls;
	      cum_statsp->height = bt_statsp->height;
	      cum_statsp->keys = bt_statsp->keys;
	      cum_statsp->ukeys = bt_statsp->ukeys;
	      cum_statsp->key_type = bt_statsp->key_type;
	      cum_statsp->key_size = bt_statsp->key_size;
	      /* alloc pkeys[] within the current optimizer environment */
	      if (cum_statsp->pkeys)
		{
		  DEALLOCATE (env, cum_statsp->pkeys);	/* free alloced */
		}
	      cum_statsp->pkeys =
		ALLOC_ATTR_CUM_STATS_PKEYS (env, cum_statsp->key_size);
	      if (!cum_statsp->pkeys)
		{
		  /* already, error has been set */
		  qo_free_attr_info (env, attr_infop);
		  return NULL;
		}
	      for (j = 0; j < cum_statsp->key_size; j++)
		{
		  cum_statsp->pkeys[j] = bt_statsp->pkeys[j];
		}

	      /* immediately return the allocated QO_ATTR_INFO */
	      return attr_infop;
	    }
	  else
	    {
	      /* keep cumulative totals of index statistics */
	      cum_statsp->leafs += bt_statsp->leafs;
	      cum_statsp->pages += bt_statsp->pages;
	      cum_statsp->oids += bt_statsp->oids;
	      cum_statsp->nulls += bt_statsp->nulls;
	      /* Assume that the key distributions overlap here, so that the
	         number of distinct keys in all of the attributes equal to the
	         maximum number of distinct keys in any one of the attributes.
	         This is probably not far from the truth; it is almost
	         certainly a better guess than assuming that all key ranges
	         are distinct. */
	      cum_statsp->height =
		MAX (cum_statsp->height, bt_statsp->height);
	      if (cum_statsp->key_size == 0 ||	/* the first found */
		  cum_statsp->keys < bt_statsp->keys)
		{
		  cum_statsp->keys = bt_statsp->keys;
		  cum_statsp->ukeys = bt_statsp->ukeys;
		  cum_statsp->key_type = bt_statsp->key_type;
		  cum_statsp->key_size = bt_statsp->key_size;
		  /* alloc pkeys[] within the current optimizer environment */
		  if (cum_statsp->pkeys)
		    {
		      DEALLOCATE (env, cum_statsp->pkeys);	/* free alloced */
		    }
		  cum_statsp->pkeys =
		    ALLOC_ATTR_CUM_STATS_PKEYS (env, cum_statsp->key_size);
		  if (!cum_statsp->pkeys)
		    {
		      /* already, error has been set */
		      qo_free_attr_info (env, attr_infop);
		      return NULL;
		    }
		  for (j = 0; j < cum_statsp->key_size; j++)
		    {
		      cum_statsp->pkeys[j] = bt_statsp->pkeys[j];
		    }
		}
	    }
	}
      else
	{
	  /* dynamic classes spec, etc
	     for example: select ... from (x, y, z) p
	     select ... from x p
	   */

	  /* keep cumulative totals of index statistics */
	  cum_statsp->leafs += bt_statsp->leafs;
	  cum_statsp->pages += bt_statsp->pages;
	  cum_statsp->oids += bt_statsp->oids;
	  cum_statsp->nulls += bt_statsp->nulls;
	  /* Assume that the key distributions overlap here, so that the
	     number of distinct keys in all of the attributes equal to the
	     maximum number of distinct keys in any one of the attributes.
	     This is probably not far from the truth; it is almost certainly
	     a better guess than assuming that all key ranges are distinct. */
	  cum_statsp->height = MAX (cum_statsp->height, bt_statsp->height);
	  if (cum_statsp->key_size == 0 ||	/* the first found */
	      cum_statsp->keys < bt_statsp->keys)
	    {
	      cum_statsp->keys = bt_statsp->keys;
	      cum_statsp->ukeys = bt_statsp->ukeys;
	      cum_statsp->key_type = bt_statsp->key_type;
	      cum_statsp->key_size = bt_statsp->key_size;
	      /* alloc pkeys[] within the current optimizer environment */
	      if (cum_statsp->pkeys)
		{
		  DEALLOCATE (env, cum_statsp->pkeys);	/* free alloced */
		}
	      cum_statsp->pkeys =
		ALLOC_ATTR_CUM_STATS_PKEYS (env, cum_statsp->key_size);
	      if (!cum_statsp->pkeys)
		{
		  /* already, error has been set */
		  qo_free_attr_info (env, attr_infop);
		  return NULL;
		}
	      for (j = 0; j < cum_statsp->key_size; j++)
		{
		  cum_statsp->pkeys[j] = bt_statsp->pkeys[j];
		}
	    }
	}

    }				/* for (i = 0; i < n; ...) */

  /* return the allocated QO_ATTR_INFO */
  return attr_infop;
}

/*
 * qo_free_attr_info () - Free the vector and any internally allocated
 *			  structures
 *   return: nothing
 *   env(in): The current optimizer environment
 *   info(in): A pointer to a previously allocated info vector
 */
static void
qo_free_attr_info (QO_ENV * env, QO_ATTR_INFO * info)
{
  QO_ATTR_CUM_STATS *cum_statsp;

  if (info)
    {
      cum_statsp = &info->cum_stats;
      if (cum_statsp->pkeys)
	{
	  DEALLOCATE (env, cum_statsp->pkeys);
	}
      DEALLOCATE (env, info);
    }

}

/*
 * qo_get_index_info () - Get index statistical information
 *   return:
 *   env(in): The current optimizer environment
 *   node(in): A join graph node
 */
static void
qo_get_index_info (QO_ENV * env, QO_NODE * node)
{
  QO_NODE_INDEX *node_indexp;
  QO_NODE_INDEX_ENTRY *ni_entryp;
  QO_INDEX_ENTRY *index_entryp;
  QO_ATTR_CUM_STATS *cum_statsp;
  QO_SEGMENT *segp;
  QO_NODE *seg_node;
  QO_CLASS_INFO_ENTRY *class_info_entryp;
  const char *name;
  int attr_id, n_attrs;
  ATTR_STATS *attr_statsp;
  BTREE_STATS *bt_statsp;
  int i, j, k;
  bool is_unique_index;

  /* pointer to QO_NODE_INDEX structure of QO_NODE */
  node_indexp = QO_NODE_INDEXES (node);

  /* for each index list(linked list of QO_INDEX_ENTRY) rooted at the node
     (all elements of QO_NODE_INDEX_ENTRY[] array) */
  for (i = 0, ni_entryp = QO_NI_ENTRY (node_indexp, 0);
       i < QO_NI_N (node_indexp); i++, ni_entryp++)
    {
      cum_statsp = &(ni_entryp)->cum_stats;
      cum_statsp->is_indexed = true;

      /* The linked list of QO_INDEX_ENTRY was built by 'qo_find_node_index()'
         function. It is the list of compatible indexes under class
         hierarchy. */
      /* for each index entry(QO_INDEX_ENTRY) on the list, acquire
         the statistics and cumulate them */
      for (j = 0, index_entryp = (ni_entryp)->head;
	   index_entryp != NULL; j++, index_entryp = index_entryp->next)
	{

	  /* The index information is associated with the first attribute of
	     index keys in the case of multi-column index and 'seg_idx[]'
	     array of QO_INDEX_ENTRY structure was built by
	     'qo_find_index_seg_and_term()' function to keep the order of
	     index key attributes. So, 'seg_idx[0]' is the right segment
	     denoting the attribute that contains the index statisitcs that
	     we want to get. */
	  segp = QO_ENV_SEG (env, (index_entryp->seg_idxs[0]));

	  /* QO_NODE of the given segment */
	  seg_node = QO_SEG_HEAD (segp);

	  /* actual attribute name of the given segment */
	  name = QO_SEG_NAME (segp);

	  /* pointer to QO_CLASS_INFO_ENTRY[] array of the node */
	  class_info_entryp = &QO_NODE_INFO (seg_node)->info[j];

	  attr_id = sm_att_id (class_info_entryp->mop, name);

	  /* pointer to ATTR_STATS of CLASS_STATS of QO_CLASS_INFO_ENTRY */
	  attr_statsp = QO_GET_CLASS_STATS (class_info_entryp)->attr_stats;

	  /* search the attribute from the class information */
	  n_attrs = QO_GET_CLASS_STATS (class_info_entryp)->n_attrs;
	  for (k = 0; k < n_attrs; k++, attr_statsp++)
	    {
	      if (attr_statsp->id == attr_id)
		{
		  break;
		}
	    }
	  if (k >= n_attrs)	/* not found */
	    {
	      attr_statsp = NULL;
	    }
	  index_entryp->stats = attr_statsp;
	  index_entryp->bt_stats_idx = -1;

	  if (!attr_statsp)
	    {
	      /* absence of the attribute statistics? */
	      continue;
	    }

	  /* if the attribute is numeric type so its min/max values are
	     meaningful, keep the min/max existing values */
	  if (DB_NUMERIC_TYPE (attr_statsp->type))
	    {

	      if (!cum_statsp->valid_limits)
		{

		  /* first time */
		  cum_statsp->type = attr_statsp->type;
		  cum_statsp->valid_limits = true;
		  /* assign values, bitwise-copy of DB_DATA structure */
		  cum_statsp->min_value = attr_statsp->min_value;
		  cum_statsp->max_value = attr_statsp->max_value;

		}
	      else
		{		/* if (!cum_statsp->valid_limits) */

		  /* compare with previsous values */
		  if (qo_data_compare (&attr_statsp->min_value,
				       &cum_statsp->min_value,
				       cum_statsp->type) < 0)
		    {
		      cum_statsp->min_value = attr_statsp->min_value;
		    }
		  if (qo_data_compare (&attr_statsp->max_value,
				       &cum_statsp->max_value,
				       cum_statsp->type) > 0)
		    {
		      cum_statsp->max_value = attr_statsp->max_value;
		    }
		  /* 'qo_data_compare()' is a simplized function that works
		     with DB_DATA instead of DB_VALUE. However, this way
		     would be enough to get minimum/maximum existing value,
		     because the values are meaningful only when their types
		     are numeric and we are considering compatible indexes
		     under class hierarchy. */

		}		/* if (!cum_statsp->valid_limits) */

	    }			/* if (DB_NUMERIC_TYPE(attr_statsp->type)) */


	  /* find the index that we are interesting within BTREE_STATS[] array */
	  for (k = 0, bt_statsp = attr_statsp->bt_stats; k <
	       attr_statsp->n_btstats; k++, bt_statsp++)
	    {

	      if (BTID_IS_EQUAL (&bt_statsp->btid, &(index_entryp->btid)))
		{
		  index_entryp->bt_stats_idx = k;
		  break;
		}

	    }			/* for (k = 0, ...) */
	  if (k == attr_statsp->n_btstats)
	    {
	      /* cannot find index in this attribute. what happens? */
	      continue;
	    }

	  if (QO_NODE_ENTITY_SPEC (node)->info.spec.only_all == PT_ALL)
	    {
	      /* class hierarchy spec
	         for example: select ... from all p */

	      /* check index uniqueness */
	      is_unique_index = false;	/* init */
	      if (SM_IS_CONSTRAINT_UNIQUE_FAMILY (index_entryp->type))
		{
		  is_unique_index = true;
		}

	      if (is_unique_index)
		{
		  /* is class hierarchy index: set unique index statistics */
		  cum_statsp->leafs = bt_statsp->leafs;
		  cum_statsp->pages = bt_statsp->pages;
		  cum_statsp->oids = bt_statsp->oids;
		  cum_statsp->nulls = bt_statsp->nulls;
		  cum_statsp->height = bt_statsp->height;
		  cum_statsp->keys = bt_statsp->keys;
		  cum_statsp->ukeys = bt_statsp->ukeys;
		  cum_statsp->key_type = bt_statsp->key_type;
		  cum_statsp->key_size = bt_statsp->key_size;
		  /* alloc pkeys[] within the current optimizer environment */
		  if (cum_statsp->pkeys)
		    {
		      DEALLOCATE (env, cum_statsp->pkeys);	/* free alloced */
		    }
		  cum_statsp->pkeys =
		    ALLOC_ATTR_CUM_STATS_PKEYS (env, cum_statsp->key_size);
		  if (!cum_statsp->pkeys)
		    {
		      /* already, error has been set */
		      return;	/* give up */
		    }
		  for (k = 0; k < cum_statsp->key_size; k++)
		    {
		      cum_statsp->pkeys[k] = bt_statsp->pkeys[k];
		    }

		  /* immediately finish getting index statistics */
		  return;
		}
	      else
		{
		  /* keep cumulative totals of index statistics */
		  cum_statsp->leafs += bt_statsp->leafs;
		  cum_statsp->pages += bt_statsp->pages;
		  cum_statsp->oids += bt_statsp->oids;
		  cum_statsp->nulls += bt_statsp->nulls;
		  /* Assume that the key distributions overlap here, so that
		     the number of distinct keys in all of the attributes
		     equal to the maximum number of distinct keys in any one
		     of the attributes. This is probably not far from the
		     truth; it is almost certainly a better guess than
		     assuming that all key ranges are distinct. */
		  cum_statsp->height =
		    MAX (cum_statsp->height, bt_statsp->height);
		  if (cum_statsp->key_size == 0 ||	/* the first found */
		      cum_statsp->keys < bt_statsp->keys)
		    {
		      cum_statsp->keys = bt_statsp->keys;
		      cum_statsp->ukeys = bt_statsp->ukeys;
		      cum_statsp->key_type = bt_statsp->key_type;
		      cum_statsp->key_size = bt_statsp->key_size;
		      /* alloc pkeys[] within the current optimizer environment */
		      if (cum_statsp->pkeys)
			{
			  DEALLOCATE (env, cum_statsp->pkeys);	/* free alloced */
			}
		      cum_statsp->pkeys =
			ALLOC_ATTR_CUM_STATS_PKEYS (env,
						    cum_statsp->key_size);
		      if (!cum_statsp->pkeys)
			{
			  /* already, error has been set */
			  return;	/* give up */
			}
		      for (k = 0; k < cum_statsp->key_size; k++)
			{
			  cum_statsp->pkeys[k] = bt_statsp->pkeys[k];
			}
		    }
		}
	    }
	  else
	    {
	      /* dynamic classes spec, etc
	         for example: select ... from (x, y, z) p
	         select ... from x p
	       */

	      /* keep cumulative totals of index statistics */
	      cum_statsp->leafs += bt_statsp->leafs;
	      cum_statsp->pages += bt_statsp->pages;
	      cum_statsp->oids += bt_statsp->oids;
	      cum_statsp->nulls += bt_statsp->nulls;
	      /* Assume that the key distributions overlap here, so that the
	         number of distinct keys in all of the attributes equal to the
	         maximum number of distinct keys in any one of the attributes.
	         This is probably not far from the truth; it is almost
	         certainly a better guess than assuming that all key ranges
	         are distinct. */
	      cum_statsp->height =
		MAX (cum_statsp->height, bt_statsp->height);
	      if (cum_statsp->key_size == 0 ||	/* the first found */
		  cum_statsp->keys < bt_statsp->keys)
		{
		  cum_statsp->keys = bt_statsp->keys;
		  cum_statsp->ukeys = bt_statsp->ukeys;
		  cum_statsp->key_type = bt_statsp->key_type;
		  cum_statsp->key_size = bt_statsp->key_size;
		  /* alloc pkeys[] within the current optimizer environment */
		  if (cum_statsp->pkeys)
		    {
		      DEALLOCATE (env, cum_statsp->pkeys);	/* free alloced */
		    }
		  cum_statsp->pkeys =
		    ALLOC_ATTR_CUM_STATS_PKEYS (env, cum_statsp->key_size);
		  if (!cum_statsp->pkeys)
		    {
		      /* already, error has been set */
		      return;	/* give up */
		    }
		  for (k = 0; k < cum_statsp->key_size; k++)
		    {
		      cum_statsp->pkeys[k] = bt_statsp->pkeys[k];
		    }
		}
	    }
	}			/* for (j = 0, ... ) */

    }				/* for (i = 0, ...) */

}

/*
 * qo_free_node_index_info () - Free the vector and any internally allocated
 *				structures
 *   return: nothing
 *   env(in): The current optimizer environment
 *   node_indexp(in): A pointer to QO_NODE_INDEX structure of QO_NODE
 */
static void
qo_free_node_index_info (QO_ENV * env, QO_NODE_INDEX * node_indexp)
{
  QO_NODE_INDEX_ENTRY *ni_entryp;
  QO_ATTR_CUM_STATS *cum_statsp;
  int i;

  if (node_indexp)
    {
      /* for each index list(linked list of QO_INDEX_ENTRY) rooted at the node
         (all elements of QO_NODE_INDEX_ENTRY[] array) */
      for (i = 0, ni_entryp = QO_NI_ENTRY (node_indexp, 0);
	   i < QO_NI_N (node_indexp); i++, ni_entryp++)
	{
	  cum_statsp = &(ni_entryp)->cum_stats;
	  if (cum_statsp->pkeys)
	    {
	      DEALLOCATE (env, cum_statsp->pkeys);
	    }
	}

      DEALLOCATE (env, node_indexp);
    }
}

/*
 * qo_data_compare () -
 *   return: 1, 0, -1
 *   data1(in):
 *   data2(in):
 *   type(in):
 *
 * Note: This is a simplized function that works with DB_DATA
 *      instead of DB_VALUE, which is the same function of 'qst_data_compare()'.
 */
static int
qo_data_compare (DB_DATA * data1, DB_DATA * data2, DB_TYPE type)
{
  int result;

  switch (type)
    {
    case DB_TYPE_INTEGER:
      result = (data1->i < data2->i) ? -1 : ((data1->i > data2->i) ? 1 : 0);
      break;
    case DB_TYPE_SHORT:
      result = ((data1->sh < data2->sh) ?
		-1 : ((data1->sh > data2->sh) ? 1 : 0));
      break;
    case DB_TYPE_BIGINT:
      result = ((data1->bigint < data2->bigint) ?
		-1 : ((data1->bigint > data2->bigint) ? 1 : 0));
      break;
    case DB_TYPE_FLOAT:
      result = (data1->f < data2->f) ? -1 : ((data1->f > data2->f) ? 1 : 0);
      break;
    case DB_TYPE_DOUBLE:
      result = (data1->d < data2->d) ? -1 : ((data1->d > data2->d) ? 1 : 0);
      break;
    case DB_TYPE_DATE:
      result = ((data1->date < data2->date) ?
		-1 : ((data1->date > data2->date) ? 1 : 0));
      break;
    case DB_TYPE_TIME:
      result = ((data1->time < data2->time) ?
		-1 : ((data1->time > data2->time) ? 1 : 0));
      break;
    case DB_TYPE_UTIME:
      result = ((data1->utime < data2->utime) ?
		-1 : ((data1->utime > data2->utime) ? 1 : 0));
      break;
    case DB_TYPE_DATETIME:
      if (data1->datetime.date < data2->datetime.date)
	{
	  result = -1;
	}
      else if (data1->datetime.date > data2->datetime.date)
	{
	  result = 1;
	}
      else if (data1->datetime.time < data2->datetime.time)
	{
	  result = -1;
	}
      else if (data1->datetime.time > data2->datetime.time)
	{
	  result = 1;
	}
      else
	{
	  result = 0;
	}
      break;
    case DB_TYPE_MONETARY:
      result = ((data1->money.amount < data2->money.amount) ?
		-1 : ((data1->money.amount > data2->money.amount) ? 1 : 0));
      break;
    default:
      /* not numeric type */
      result = 0;
      break;
    }

  return result;
}

/*
 * qo_estimate_statistics () - Make a wild-ass guess at the appropriate
 *			       statistics for this class.  The statistics
 *			       manager doesn't know anything about this class,
 *			       so we're on our own.
 *   return: nothing
 *   class_mop(in): The mop of the class whose statistics need to be
                    fabricated
 *   statblock(in): The CLASS_STATS structure to be populated
 */
static void
qo_estimate_statistics (MOP class_mop, CLASS_STATS * statblock)
{
  /*
   * It would be nice if we could the get the actual number of pages
   * allocated for the class; at least then we could make some sort of
   * realistic guess at the upper bound of the number of objects (we
   * can already figure out the "average" size of an object).
   *
   * Really, the statistics manager ought to be doing this on its own.
   */

  statblock->heap_size = NOMINAL_HEAP_SIZE (class_mop);
  statblock->num_objects =
    (statblock->heap_size * DB_PAGESIZE) / NOMINAL_OBJECT_SIZE (class_mop);

}

/*
 * qo_env_new () -
 *   return:
 *   parser(in):
 *   query(in):
 */
static QO_ENV *
qo_env_new (PARSER_CONTEXT * parser, PT_NODE * query)
{
  QO_ENV *env;

  env = (QO_ENV *) malloc (sizeof (QO_ENV));
  if (env == NULL)
    {
      return NULL;
    }

  env->parser = parser;
  env->pt_tree = query;
  env->nsegs = 0;
  env->nnodes = 0;
  env->nedges = 0;
  env->neqclasses = 0;
  env->nterms = 0;
  env->nsubqueries = 0;
  env->npartitions = 0;
  env->final_plan = NULL;
  env->segs = NULL;
  env->nodes = NULL;
  env->eqclasses = NULL;
  env->terms = NULL;
  env->subqueries = NULL;
  env->partitions = NULL;
  bitset_init (&(env->final_segs), env);
  env->tmp_bitset = NULL;
  env->bail_out = 0;
  env->planner = NULL;
  env->dump_enable = PRM_QO_DUMP;
  bitset_init (&(env->fake_terms), env);

  return env;
}

#if 0
/*
 * qo_malloc () - Try to allocate the requested number of bytes.  If that
 *                fails, throw to some enclosing unwind-protect handler
 *   return: void *
 *   env(in): The optimizer environment from which the request is issued
 *   size(in): The number of bytes requested
 *   file(in): The file from which qo_malloc() was called
 *   line(in): The line number of from which qo_malloc() was called
 */
void *
qo_malloc (QO_ENV * env, unsigned size, const char *file, int line)
{
  void *p;

  p = malloc (size);
  if (p == NULL)
    {
      longjmp (env->catch, 1);
    }
  return p;

}
#endif

/*
 * qo_abort () -
 *   return:
 *   env(in):
 *   file(in):
 *   line(in):
 */
void
qo_abort (QO_ENV * env, const char *file, int line)
{
  er_set (ER_WARNING_SEVERITY, file, line, ER_QO_FAILED_ASSERTION, 0);
  longjmp (env->catch_, 2);
}

/*
 * qo_env_free () -
 *   return:
 *   env(in):
 */
void
qo_env_free (QO_ENV * env)
{
  if (env)
    {
      int i;

      /*
       * Be sure to use Nnodes, Nterms, and Nsegs as the loop limits in
       * the code below, because those are the sizes of the allocated
       * arrays; nnodes, nterms, and nsegs are the extents of those
       * arrays that were actually used, but the entries past those
       * extents need to be cleaned up too.
       */

      if (env->segs)
	{
	  for (i = 0; i < env->Nsegs; ++i)
	    {
	      qo_seg_free (QO_ENV_SEG (env, i));
	    }
	  DEALLOCATE (env, env->segs);
	}

      if (env->nodes)
	{
	  for (i = 0; i < env->Nnodes; ++i)
	    {
	      qo_node_free (QO_ENV_NODE (env, i));
	    }
	  DEALLOCATE (env, env->nodes);
	}

      if (env->eqclasses)
	{
	  for (i = 0; i < env->neqclasses; ++i)
	    {
	      qo_eqclass_free (QO_ENV_EQCLASS (env, i));
	    }
	  DEALLOCATE (env, env->eqclasses);
	}

      if (env->terms)
	{
	  for (i = 0; i < env->Nterms; ++i)
	    {
	      qo_term_free (QO_ENV_TERM (env, i));
	    }
	  DEALLOCATE (env, env->terms);
	}

      if (env->partitions)
	{
	  for (i = 0; i < env->npartitions; ++i)
	    {
	      qo_partition_free (QO_ENV_PARTITION (env, i));
	    }
	  DEALLOCATE (env, env->partitions);
	}

      if (env->subqueries)
	{
	  for (i = 0; i < env->nsubqueries; ++i)
	    {
	      qo_subquery_free (&env->subqueries[i]);
	    }
	  DEALLOCATE (env, env->subqueries);
	}

      bitset_delset (&(env->final_segs));
      bitset_delset (&(env->fake_terms));

      if (env->planner)
	{
	  qo_planner_free (env->planner);
	}

      free_and_init (env);
    }
}

/*
 * qo_exchange () -
 *   return:
 *   t0(in):
 *   t1(in):
 */
static void
qo_exchange (QO_TERM * t0, QO_TERM * t1)
{

  /*
   * 'env' attribute is the same in both, don't bother with it.
   */
  TERMCLASS_EXCHANGE (t0->term_class, t1->term_class);
  BISET_EXCHANGE (t0->nodes, t1->nodes);
  BISET_EXCHANGE (t0->segments, t1->segments);
  DOUBLE_EXCHANGE (t0->selectivity, t1->selectivity);
  INT_EXCHANGE (t0->rank, t1->rank);
  PT_NODE_EXCHANGE (t0->pt_expr, t1->pt_expr);
  INT_EXCHANGE (t0->location, t1->location);
  BISET_EXCHANGE (t0->subqueries, t1->subqueries);
  JOIN_TYPE_EXCHANGE (t0->join_type, t1->join_type);
  INT_EXCHANGE (t0->can_use_index, t1->can_use_index);
  SEGMENTPTR_EXCHANGE (t0->index_seg[0], t1->index_seg[0]);
  SEGMENTPTR_EXCHANGE (t0->index_seg[1], t1->index_seg[1]);
  SEGMENTPTR_EXCHANGE (t0->seg, t1->seg);
  SEGMENTPTR_EXCHANGE (t0->oid_seg, t1->oid_seg);
  NODEPTR_EXCHANGE (t0->head, t1->head);
  NODEPTR_EXCHANGE (t0->tail, t1->tail);
  EQCLASSPTR_EXCHANGE (t0->eqclass, t1->eqclass);
  SEGMENTPTR_EXCHANGE (t0->nominal_seg, t1->nominal_seg);
  FLAG_EXCHANGE (t0->flag, t1->flag);
  /*
   * DON'T exchange the 'idx' values!
   */
}

/*
 * qo_discover_edges () -
 *   return:
 *   env(in):
 */
static void
qo_discover_edges (QO_ENV * env)
{
  int i, j, n;
  QO_TERM *term, *edge, *edge2;
  QO_NODE *node;
  PT_NODE *pt_expr;
  int t;
  BITSET_ITERATOR iter;
  BITSET direct_nodes;
  int t1, t2;
  QO_TERM *term1, *term2;

  bitset_init (&direct_nodes, env);

  i = 0;
  n = env->nterms;

  while (i < n)
    {
      term = QO_ENV_TERM (env, i);
      if (QO_IS_EDGE_TERM (term))
	{
	  ++env->nedges;
	  ++i;
	}
      else
	{
	  if (i < --n)
	    {
	      /*
	       * Exchange the terms at the two boundaries.  This moves
	       * a known non-edge up to just below the section of other
	       * non-edge terms, and moves a term of unknown "edgeness"
	       * down to just above the section of known edges.  Leave
	       * the bottom boundary alone, but move the upper boundary
	       * down one notch.
	       */
	      qo_exchange (QO_ENV_TERM (env, i), QO_ENV_TERM (env, n));
	    }
	}
    }

  /* sort join-term on selectivity as descending order */
  for (t1 = 0; t1 < i - 1; t1++)
    {
      term1 = QO_ENV_TERM (env, t1);
      for (t2 = t1 + 1; t2 < i; t2++)
	{
	  term2 = QO_ENV_TERM (env, t2);
	  if (QO_TERM_SELECTIVITY (term1) < QO_TERM_SELECTIVITY (term2))
	    {
	      qo_exchange (term1, term2);
	    }
	}
    }
  /* sort sarg-term on selectivity as descending order */
  for (t1 = i; t1 < env->nterms - 1; t1++)
    {
      term1 = QO_ENV_TERM (env, t1);
      for (t2 = t1 + 1; t2 < env->nterms; t2++)
	{
	  term2 = QO_ENV_TERM (env, t2);
	  if (QO_TERM_SELECTIVITY (term1) < QO_TERM_SELECTIVITY (term2))
	    {
	      qo_exchange (term1, term2);
	    }
	}
    }

  for (n = env->nterms; i < n; ++i)
    {
      term = QO_ENV_TERM (env, i);
      if (QO_TERM_CLASS (term) == QO_TC_SARG)
	{
	  QO_ASSERT (env, bitset_cardinality (&(QO_TERM_NODES (term))) == 1);
	  qo_node_add_sarg (QO_ENV_NODE (env,
					 bitset_first_member (&(QO_TERM_NODES
								(term)))),
			    term);
	}
    }

  /*
   * Check some invariants.  If something has gone wrong during the
   * discovery phase to violate these invariants, it will mean certain
   * death for later phases, so we need to discover it now while it's
   * convenient.
   */
  for (i = 0, n = env->nedges; i < n; ++i)
    {
      edge = QO_ENV_TERM (env, i);
      QO_ASSERT (env, QO_TERM_HEAD (edge) != NULL
		 && QO_TERM_TAIL (edge) != NULL);

      if (QO_TERM_JOIN_TYPE (edge) != JOIN_INNER
	  && QO_TERM_CLASS (edge) != QO_TC_JOIN)
	{
	  for (j = 0; j < n; j++)
	    {
	      edge2 = QO_ENV_TERM (env, j);
	      if (i != j
		  && bitset_is_equivalent (&(QO_TERM_NODES (edge)),
					   &(QO_TERM_NODES (edge2))))
		{
		  QO_TERM_JOIN_TYPE (edge2) = QO_TERM_JOIN_TYPE (edge);
		}
	    }
	}

      pt_expr = QO_TERM_PT_EXPR (edge);

      /* check for always true transitive join term */
      if (pt_expr
	  && PT_EXPR_INFO_IS_FLAGED (pt_expr, PT_EXPR_INFO_TRANSITIVE))
	{

	  BITSET_CLEAR (direct_nodes);

	  for (j = 0; j < n; j++)
	    {
	      edge2 = QO_ENV_TERM (env, j);
	      if (bitset_intersects
		  (&(QO_TERM_NODES (edge2)), &(QO_TERM_NODES (edge))))
		{
		  bitset_union (&direct_nodes, &(QO_TERM_NODES (edge2)));
		}
	    }			/* for (j = 0; ...) */

	  /* check for direct connected nodes */
	  for (t = bitset_iterate (&direct_nodes, &iter); t != -1;
	       t = bitset_next_member (&iter))
	    {
	      node = QO_ENV_NODE (env, t);
	      if (!QO_NODE_SARGABLE (node))
		{
		  break;	/* give up */
		}
	    }

	  /* found dummy join edge. it is used for planning only */
	  if (t == -1)
	    {
	      QO_TERM_CLASS (edge) = QO_TC_DUMMY_JOIN;

	      /* keep out from m-join edge */
	      QO_TERM_CLEAR_FLAG (edge, QO_TERM_MERGEABLE_EDGE);
	    }
	}
    }

  bitset_delset (&direct_nodes);
}

/*
 * qo_find_index_terms () - Find the terms which contain the passed segments
 *   return:
 *   env(in): The environment used
 *   segsp(in): Passed BITSET of interested segments
 *   termsp(in): Returned BITSET of terms which contain the segments
 */
static void
qo_find_index_terms (QO_ENV * env, BITSET * segsp, BITSET * termsp)
{
  int t;
  QO_TERM *qo_termp;

  BITSET_CLEAR (*termsp);

  /* traverse all terms */
  for (t = 0; t < env->nterms; t++)
    {
      /* get the pointer to QO_TERM structure */
      qo_termp = QO_ENV_TERM (env, t);

      /* Fake terms (e.g., dependency links) won't have pt_expr's associated
         with them. They can't be implemented as indexed sargs, either,
         so don't worry about them here. */
      if (!QO_TERM_PT_EXPR (qo_termp))
	{
	  continue;
	}
      /* 'analyze_term()' function verifies that all indexable
         terms are expression so that they have 'pt_expr' filed of type
         PT_EXPR. */

      /* if the segments that give rise to the term are in the given segment
         set */
      if (bitset_intersects (&(QO_TERM_SEGS (qo_termp)), segsp))
	{
	  /* collect this term */
	  bitset_add (termsp, t);
	}

    }				/* for (t = 0; t < env->nterms; t++) */

}

/*
 * qo_find_index_seg_terms () - Find the terms which contain the passed segment.
 *                               Only indexable and SARG terms are included
 *   return:
 *   env(in): The environment used
 *   seg_idx(in): Passed idx of an interested segment
 *   seg_equal_termsp(in): Returned BITSET of equal erms which contain
 *			   the segments and its class is TC_SARG
 *   seg_other_termsp(in):
 */
static void
qo_find_index_seg_terms (QO_ENV * env, int seg_idx,
			 BITSET * seg_equal_termsp, BITSET * seg_other_termsp)
{
  int t;
  QO_TERM *qo_termp;

  BITSET_CLEAR (*seg_equal_termsp);
  BITSET_CLEAR (*seg_other_termsp);

  /* traverse all terms */
  for (t = 0; t < env->nterms; t++)
    {
      /* get the pointer to QO_TERM structure */
      qo_termp = QO_ENV_TERM (env, t);

      /* ignore this term if it is not marked as indexable by
         'analyze_term()' */
      if (!qo_termp->can_use_index)
	{
	  continue;
	}

      /* Fake terms (e.g., dependency links) won't have pt_expr's associated
         with them. They can't be implemented as indexed sargs, either,
         so don't worry about them here. */
      if (!QO_TERM_PT_EXPR (qo_termp))
	{
	  continue;
	}
      /* 'analyze_term()' function verifies that all indexable
         terms are expression so that they have 'pt_expr' filed of type
         PT_EXPR. */

      /* if the term is sarg and the given segment is involed in the
         expression that gives rise to the term */
      if (QO_TERM_CLASS (qo_termp) == QO_TC_SARG
	  && BITSET_MEMBER (QO_TERM_SEGS (qo_termp), seg_idx))
	{
	  /* collect this term */
	  if (QO_TERM_IS_FLAGED (qo_termp, QO_TERM_EQUAL_OP))
	    {
	      bitset_add (seg_equal_termsp, t);
	    }
	  else
	    {
	      bitset_add (seg_other_termsp, t);
	    }
	}
    }
}

/*
 * is_equivalent_indexes () - Compare the two index entries
 *   return: True/False
 *   index1(in): First index entry
 *   index2(in): Second index entry
 *
 * Note: Return true if they are equivalent
 *     and false otherwise.  In order to be equivalent, the index entries
 *     must contain the same segments specified in the same order
 */
static int
is_equivalent_indexes (QO_INDEX_ENTRY * index1, QO_INDEX_ENTRY * index2)
{
  int i, equivalent;

  /*
   *  If the number of segments is different, then the indexes can't
   *  be equivalent (cheap test).
   */
  if (index1->nsegs != index2->nsegs)
    {
      return false;
    }

  /*
   * Now compare the two indexes element by element
   */
  equivalent = true;
  for (i = 0; i < index1->nsegs; i++)
    {
      if ((index1->seg_idxs[i]) != (index2->seg_idxs[i]))
	{
	  equivalent = false;
	  break;
	}
    }

  return equivalent;
}

/*
 * qo_find_matching_index () -
 *   return: int (index of matching index entry, or -1)
 *   index_entry(in): Index entry to match
 *   class_indexes(in): Array of index entries to search
 *
 * Note:
 *     Given a index entry, search the index array looking for a match.
 *     The array index of the matching entry is returned (if found).
 *     A -1 is returned if a matching entry is not found.
 *
 *     Indexes which are already a part of a heirarchical compatible index
 *     list are not considered (these are identifialbe since their next
 *     pointer is non-NULL).
 */
static int
qo_find_matching_index (QO_INDEX_ENTRY * index_entry,
			QO_INDEX * class_indexes)
{
  int i;

  for (i = 0; i < class_indexes->n; i++)
    {
      /*
       *  A matching index is found if the index node is not already a member
       *  of a heirarchical compatible index list (i.e. next pointer is NULL)
       *  and if it matches the passes <index_entry>.
       */
      if (QO_INDEX_INDEX (class_indexes, i)->next == NULL
	  && is_equivalent_indexes (index_entry,
				    QO_INDEX_INDEX (class_indexes, i)))
	{
	  break;
	}
    }

  /*
   *  If a match is found, return its index, otherwise return -1
   */
  if (i < class_indexes->n)
    {
      return i;
    }
  else
    {
      return -1;
    }
}

/*
 * is_index_compatible () -
 *   return: int (True/False)
 *   class_info(in): Class info structure
 *   n(in): Index into class info structure.  This determines the level
 *          in the class heirarchy that we're currently concerned with
 *   index_entry(in): Index entry to match against
 *
 * Note:
 *     This is a recursive function which is used to verify that a
 *     given index entry is compatible accross the class heirarchy.
 *     An index entry is compatible if there exists an index definition
 *     on the same sequence of attributes at each level in the class
 *     heirarchy.  If the index entry is compatible, the entry will be
 *     marked as such throughout the hierarchy.
 */
static QO_INDEX_ENTRY *
is_index_compatible (QO_CLASS_INFO * class_info, int n,
		     QO_INDEX_ENTRY * index_entry)
{
  QO_CLASS_INFO_ENTRY *class_entry;
  QO_INDEX *class_indexes;
  QO_INDEX_ENTRY *index;
  int i;

  if (n >= class_info->n)
    {
      return NULL;
    }

  class_entry = &(class_info->info[n]);
  class_indexes = class_entry->index;

  i = qo_find_matching_index (index_entry, class_indexes);
  if (i < 0)
    {
      return NULL;
    }

  index = QO_INDEX_INDEX (class_indexes, i);
  if (n == (class_info->n - 1))
    {
      index->next = NULL;
      return index;
    }
  else
    {
      index->next = is_index_compatible (class_info, n + 1, index);
      if (index->next == NULL)
	{
	  return NULL;
	}
      else
	{
	  return index;
	}
    }

/*  return NULL;*/
}

/*
 * qo_find_index_segs () -
 *   return:
 *   env(in):
 *   consp(in):
 *   nodep(in):
 *   seg_idx(in):
 *   seg_idx_num(in):
 *   nseg_idxp(in):
 *   segs(in):
 */
static bool
qo_find_index_segs (QO_ENV * env,
		    SM_CLASS_CONSTRAINT * consp, QO_NODE * nodep,
		    int *seg_idx, int seg_idx_num, int *nseg_idxp,
		    BITSET * segs)
{
  QO_SEGMENT *segp;
  SM_ATTRIBUTE *attrp;
  BITSET working;
  BITSET_ITERATOR iter;
  int i, iseg;
  bool matched;

  /* working set; indexed segments */
  bitset_init (&working, env);
  bitset_assign (&working, &(QO_NODE_SEGS (nodep)));

  /* for each attribute of this constraint */
  for (i = 0; consp->attributes[i] && *nseg_idxp < seg_idx_num; i++)
    {

      attrp = consp->attributes[i];

      matched = false;
      /* for each indexed segments of this node, compare the name of the
         segment with the one of the attribute */
      for (iseg = bitset_iterate (&working, &iter);
	   iseg != -1; iseg = bitset_next_member (&iter))
	{

	  segp = QO_ENV_SEG (env, iseg);

	  if (!intl_mbs_casecmp (QO_SEG_NAME (segp), attrp->header.name))
	    {

	      bitset_add (segs, iseg);	/* add the segment to the index segment set */
	      bitset_remove (&working, iseg);	/* remove the segment from the working set */
	      seg_idx[i] = iseg;	/* remember the order of the index segments */
	      (*nseg_idxp)++;	/* number of index segments, 'seg_idx[]' */
	      /* If we're handling with a multi-column index, then only
	         equality expressions are allowed except for the last
	         matching segment. */
	      matched = true;
	      break;
	    }			/* if (!intl_mbs_casecmp...) */

	}			/* for (iseg = bitset_iterate(&working, &iter); ...) */

      if (!matched)
	{
	  seg_idx[i] = -1;	/* not found matched segment */
	  (*nseg_idxp)++;	/* number of index segments, 'seg_idx[]' */
	}			/* if (!matched) */

    }				/* for (i = 0; consp->attributes[i]; i++) */

  bitset_delset (&working);

  return (seg_idx[0] != -1) ? true : false;
  /* this index is feasible to use if at least the first attribute of index
     is specified(matched) */
}

/*
 * qo_find_node_indexes () -
 *   return:
 *   env(in): The environment to be updated
 *   nodep(in): The node to be updated
 *
 * Note: Scan the class constraints associated with the node.  If
 *              a match is found between a class constraint and the
 *              segments, then add an QO_INDEX to the node.  A match
 *              occurs when the class constraint attribute are a subset
 *              of the segments.  We currently consider SM_CONSTRAINT_INDEX
 *              and SM_CONSTRAINT_UNIQUE constraint types.
 */
static void
qo_find_node_indexes (QO_ENV * env, QO_NODE * nodep)
{
  int i, j, n, col_num;
  QO_CLASS_INFO *class_infop;
  QO_CLASS_INFO_ENTRY *class_entryp;
  QO_USING_INDEX *uip;
  QO_INDEX *indexp;
  QO_INDEX_ENTRY *index_entryp;
  QO_NODE_INDEX *node_indexp;
  QO_NODE_INDEX_ENTRY *ni_entryp;
  SM_CLASS_CONSTRAINT *constraints, *consp;
  int *seg_idx, seg_idx_arr[NELEMENTS], nseg_idx;
  bool found;
  BITSET index_segs, index_terms;

  /* information of classes underlying this node */
  class_infop = QO_NODE_INFO (nodep);

  if (class_infop->n <= 0)
    {
      return;			/* no classes, nothing to do process */
    }


  /* for each class in the hierarachy, search the class constraint cache
     looking for applicable indexes(UNIQUE and INDEX constraint) */
  for (i = 0; i < class_infop->n; i++)
    {

      /* class information entry */
      class_entryp = &(class_infop->info[i]);
      /* get constraints of the class */
      constraints = sm_class_constraints (class_entryp->mop);

      /* count the number of INDEX and UNIQUE constraints contatined in this
         class */
      n = 0;
      for (consp = constraints; consp; consp = consp->next)
	{
	  if (SM_IS_CONSTRAINT_INDEX_FAMILY (consp->type))
	    {
	      n++;
	    }
	}
      /* allocate room for the constraint indexes */
      /* we don't have apriori knowledge about which constraints will be
         applied, so allocate room for all of them */
      /* qo_alloc_index(env, n) will allocate QO_INDEX structure and
         QO_INDEX_ENTRY structure array */
      indexp = class_entryp->index = qo_alloc_index (env, n);

      indexp->n = 0;

      /* for each constraint of the class */
      for (consp = constraints; consp; consp = consp->next)
	{

	  if (!SM_IS_CONSTRAINT_INDEX_FAMILY (consp->type))
	    {
	      continue;		/* neither INDEX nor UNIQUE constraint, skip */
	    }

	  uip = QO_NODE_USING_INDEX (nodep);
	  j = -1;
	  if (uip)
	    {
	      if (QO_UI_N (uip) == 0)
		{
		  /* USING INDEX NONE case */
		  continue;
		}
	      /* search USING INDEX list */
	      found = false;
	      for (j = 0; j < QO_UI_N (uip); j++)
		{
		  if (!intl_mbs_casecmp (consp->name, QO_UI_INDEX (uip, j)))
		    {
		      found = true;
		      break;
		    }
		}
	      if (QO_UI_FORCE (uip, 0) == -2)
		{
		  /* USING INDEX ALL EXCEPT case */
		  if (found)
		    {
		      /* this constraint(index) is specified in
		         USING INDEX ALL EXCEPT clause; do not use it */
		      continue;
		    }
		  j = -1;
		}
	      else
		{		/* QO_UI_FORCE(uip, j) could be either -1, 0, or 1 */
		  /* normal USING INDEX case */
		  if (!found)
		    {
		      /* this constraint(index) is not specified in
		         USING INDEX clause; do not use it */
		      continue;
		    }
		}
	    }

	  bitset_init (&index_segs, env);
	  bitset_init (&index_terms, env);
	  nseg_idx = 0;

	  /* count the number of columns on this constraint */
	  for (col_num = 0; consp->attributes[col_num]; col_num++)
	    {
	      ;
	    }
	  if (col_num <= NELEMENTS)
	    {
	      seg_idx = seg_idx_arr;
	    }
	  else
	    {
	      /* allocate seg_idx */
	      seg_idx = (int *) malloc (sizeof (int) * col_num);
	      if (seg_idx == NULL)
		{
		  /* cannot allocate seg_idx, use seg_idx_arr instead. */
		  seg_idx = seg_idx_arr;
		  col_num = NELEMENTS;
		}
	    }

	  /* find indexed segments into 'seg_idx[]' */
	  found = qo_find_index_segs (env, consp, nodep,
				      seg_idx, col_num, &nseg_idx,
				      &index_segs);
	  /* 'seg_idx[nseg_idx]' array contains index no.(idx) of the segments
	     which are found and applicable to this index(constraint) as
	     search key in the order of the index key attribute. For example,
	     if the index consists of attributes 'b' and 'a', and the given
	     segments of the node are 'a(1)', 'b(2)' and 'c(3)', then the
	     result of 'seg_idx[]' will be '{ 2, 1, -1 }'. The value -1 in
	     'seg_idx[] array means that no segment is specified. */
	  if (found == true)
	    {
	      /* if applicable index was found, add it to the node */

	      /* fill in QO_INDEX_ENTRY structure */
	      index_entryp = QO_INDEX_INDEX (indexp, indexp->n);
	      index_entryp->seg_idxs = (int *) NALLOCATE (env, int, nseg_idx);
	      index_entryp->seg_equal_terms =
		(BITSET *) NALLOCATE (env, BITSET, nseg_idx);
	      index_entryp->seg_other_terms =
		(BITSET *) NALLOCATE (env, BITSET, nseg_idx);
	      index_entryp->type = consp->type;
	      index_entryp->class_ = class_entryp;
	      index_entryp->btid = consp->index;
	      /* j == -1 iff no USING INDEX or USING INDEX ALL EXCEPT */
	      index_entryp->force = (j == -1) ? 0 : QO_UI_FORCE (uip, j);
	      index_entryp->name = consp->name;
	      index_entryp->col_num = col_num;
	      index_entryp->stats = NULL;
	      index_entryp->bt_stats_idx = -1;

	      index_entryp->nsegs = nseg_idx;
	      /* assign seg_idx[] and seg_terms[] */
	      for (j = 0; j < nseg_idx; j++)
		{
		  bitset_init (&(index_entryp->seg_equal_terms[j]), env);
		  bitset_init (&(index_entryp->seg_other_terms[j]), env);
		  index_entryp->seg_idxs[j] = seg_idx[j];
		  if (index_entryp->seg_idxs[j] != -1)
		    {
		      qo_find_index_seg_terms (env, seg_idx[j],
					       &(index_entryp->
						 seg_equal_terms[j]),
					       &(index_entryp->
						 seg_other_terms[j]));
		    }
		}		/* for (j = 0; j < nseg_idx; j++) */
	      qo_find_index_terms (env, &index_segs, &(index_entryp->terms));

	      (indexp->n)++;

	    }

	  bitset_delset (&(index_segs));
	  bitset_delset (&(index_terms));
	  if (seg_idx != seg_idx_arr)
	    {
	      free_and_init (seg_idx);
	    }

	}			/* for (consp = constraintp; consp; consp = consp->next) */

    }				/* for (i = 0; i < class_infop->n; i++) */
  /* class_infop->n >= 1 */

  /* find and mark indexes which are compatible across class heirarchy */

  indexp = class_infop->info[0].index;

  /* allocate room for the compatible heirarchical indexex */
  /* We'll go ahead and allocate room for each index in the top level
     class. This is the worst case situation and it simplifies the code
     a bit. */
  /* Malloc and Init a QO_INDEX struct with n entries. */
  node_indexp =
    QO_NODE_INDEXES (nodep) =
    (QO_NODE_INDEX *) malloc (SIZEOF_NODE_INDEX (indexp->n));
  memset (node_indexp, 0, SIZEOF_NODE_INDEX (indexp->n));

  memset(node_indexp, 0, SIZEOF_NODE_INDEX (indexp->n));

  QO_NI_N (node_indexp) = 0;

  /* if we don`t have any indexes to process, we're through
     if there is only one, then make sure that the head pointer points to it
     if there are more than one, we also need to construct a linked list
     of compatible indexes by recursively searching down the heirarchy */
  for (i = 0; i < indexp->n; i++)
    {
      index_entryp = QO_INDEX_INDEX (indexp, i);
      /* get compatible(equivalent) index of the next class
         'index_entryp->next' points to it */
      index_entryp->next = is_index_compatible (class_infop, 1, index_entryp);

      if ((index_entryp->next != NULL) || (class_infop->n == 1))
	{
	  /* fill in QO_NODE_INDEX_ENTRY structure */
	  ni_entryp = QO_NI_ENTRY (node_indexp, QO_NI_N (node_indexp));
	  /* number of classes on the list */
	  (ni_entryp)->n = class_infop->n;
	  /* link QO_INDEX_ENTRY struture to QO_NODE_INDEX_ENTRY strucure */
	  (ni_entryp)->head = index_entryp;
	  QO_NI_N (node_indexp)++;
	}
    }				/* for (i = 0; i < indexp->n; i++) */

}

/*
 * qo_discover_indexes () -
 *   return: nothing
 *   env(in): The environment to be updated
 *
 * Note: Study each term to finish determination of whether it can use
 *	an index.  analyze_term() already determined whether each
 *	term qualifies structurally, and qo_get_class_info() has
 *	determined all of the indexes that are available, so all we
 *	have to do here is combine those two pieces of information.
 */
static void
qo_discover_indexes (QO_ENV * env)
{
  int i, j, k, b, n, s;
  bool found;
  BITSET_ITERATOR bi;
  QO_NODE_INDEX *node_indexp;
  QO_NODE_INDEX_ENTRY *ni_entryp;
  QO_INDEX_ENTRY *index_entryp;
  QO_TERM *termp;
  QO_NODE *nodep;
  QO_SEGMENT *segp;


  /* iterate over all nodes and find indexes for each node */
  for (i = 0; i < env->nnodes; i++)
    {

      nodep = QO_ENV_NODE (env, i);
      if (nodep->info)
	{
	  /* find indexed segments that belong to this node and get indexes
	     that apply to indexed segments */
	  qo_find_node_indexes (env, nodep);
	  /* collect statistic infomation on discovered indexes */
	  qo_get_index_info (env, nodep);
	}
      else
	{
	  /* If the 'info' of node is NULL, then this is probably a derived
	     table. Without the info, we don't have class informatino to
	     work with so we really can't do much so just skip the node. */
	  QO_NODE_INDEXES (nodep) = NULL;	/* this node will not use a index */
	}

    }				/* for (n = 0; n < env->nnodes; n++) */

  /* for each terms, look indexed segements and filter out the segments
     which don't actually contain any indexes */
  for (i = 0; i < env->nterms; i++)
    {

      termp = QO_ENV_TERM (env, i);

      /* before, 'index_seg[]' has all possible indexed segments, that is
         assigned at 'analyze_term()' */
      /* for all 'term.index_seg[]', examine if it really has index or not */
      k = 0;
      for (j = 0; j < termp->can_use_index; j++)
	{

	  segp = termp->index_seg[j];

	  found = false;	/* init */
	  /* for each nodes, do traverse */
	  for (b = bitset_iterate (&(QO_TERM_NODES (termp)), &bi);
	       b != -1 && !found; b = bitset_next_member (&bi))
	    {

	      nodep = QO_ENV_NODE (env, b);

	      /* pointer to QO_NODE_INDEX structure of QO_NODE */
	      node_indexp = QO_NODE_INDEXES (nodep);
	      if (node_indexp == NULL)
		{
		  /* node has not any index
		   * skip and go ahead
		   */
		  continue;
		}

	      /* for each index list rooted at the node */
	      for (n = 0, ni_entryp = QO_NI_ENTRY (node_indexp, 0);
		   n < QO_NI_N (node_indexp) && !found; n++, ni_entryp++)
		{

		  index_entryp = (ni_entryp)->head;

		  /* for each segments constrained by the index */
		  for (s = 0; s < index_entryp->nsegs && !found; s++)
		    {
		      if (QO_SEG_IDX (segp) == (index_entryp->seg_idxs[s]))
			{
			  /* found specified seg
			   * stop traverse
			   */
			  found = true;

			  /* record term at segment structure */
			  bitset_add (&(QO_SEG_INDEX_TERMS (segp)),
				      QO_TERM_IDX (termp));
			  /* indexed segment in 'index_seg[]' array */
			  termp->index_seg[k++] = termp->index_seg[j];
			}
		    }		/* for (s = 0 ... ) */

		}		/* for (n = 0 ... ) */

	    }			/* for (b = ... ) */

	}			/* for (j = 0 ... ) */

      termp->can_use_index = k;	/* dimension of 'index_seg[]' */
      /* clear unused, discarded 'index_seg[]' entries */
      while (k < j)
	{
	  termp->index_seg[k++] = NULL;
	}

    }				/* for (i = 0; i < env->nterms; i++) */

}

/*
 * qo_discover_partitions () -
 *   return:
 *   env(in):
 */
static void
qo_discover_partitions (QO_ENV * env)
{
  int N = env->nnodes;		/* The number of nodes in the join graph */
  int E = env->nedges;		/* The number of edges (in the strict sense) */
  int P = 0;			/* The number of partitions */
  int e, n, p;

  int *buddy;			/* buddy[i] is the index of another node in the same partition as i */
  int *partition;		/* partition[i] is the index of the partition to which node i belongs */
  BITSET_ITERATOR bi;
  int hi, ti, r;
  QO_TERM *term;
  QO_PARTITION *part;
  int M_offset, join_info_size;
  int rel_idx;

  buddy = NALLOCATE (env, int, (2 * N));
  partition = buddy + N;

  /*
   * This code assumes that there will be no ALLOCATE failures; if
   * there are, the buddy array will be lost.
   */

  for (n = 0; n < N; ++n)
    {
      buddy[n] = -1;
      partition[n] = -1;
    }

  for (e = 0; e < E; ++e)
    {
      term = QO_ENV_TERM (env, e);

      /*
       * Identify one of the nodes in this term, and run to the top of
       * the tree in which it resides.
       */
      hi = bitset_iterate (&(QO_TERM_NODES (term)), &bi);
      while (buddy[hi] != -1)
	{
	  hi = buddy[hi];
	}

      /*
       * Now buddy up all of the other nodes encompassed by this term.
       */
      while ((ti = bitset_next_member (&bi)) != -1)
	{
	  /*
	   * Run to the top of the tree in which node[ti] resides.
	   */
	  while (buddy[ti] != -1)
	    {
	      ti = buddy[ti];
	    }
	  /*
	   * Join the two trees together.
	   */
	  if (hi != ti)
	    {
	      buddy[hi] = ti;
	    }
	}
    }

  /*
   * Now assign the actual partitions.
   */
  for (n = 0; n < N; ++n)
    {
      if (partition[n] == -1)
	{
	  r = n;
	  /*
	   * Find the root of the tree to which node[n] belongs.
	   */
	  while (buddy[r] != -1)
	    {
	      r = buddy[r];
	    }
	  /*
	   * If a partition hasn't already been assigned for this tree,
	   * assign one now.
	   */
	  if (partition[r] == -1)
	    {
	      QO_PARTITION *part = QO_ENV_PARTITION (env, P);
	      QO_NODE *node = QO_ENV_NODE (env, r);
	      qo_partition_init (env, part, P);
	      bitset_add (&(QO_PARTITION_NODES (part)), r);
	      bitset_union (&(QO_PARTITION_DEPENDENCIES (part)),
			    &(QO_NODE_DEP_SET (node)));
	      QO_NODE_PARTITION (node) = part;
	      partition[r] = P;
	      P++;
	    }
	  /*
	   * Now add node[n] to that partition.
	   */
	  if (n != r)
	    {
	      QO_PARTITION *part = QO_ENV_PARTITION (env, partition[r]);
	      QO_NODE *node = QO_ENV_NODE (env, n);
	      partition[n] = partition[r];
	      bitset_add (&(QO_PARTITION_NODES (part)), n);
	      bitset_union (&(QO_PARTITION_DEPENDENCIES (part)),
			    &(QO_NODE_DEP_SET (node)));
	      QO_NODE_PARTITION (node) = part;
	    }
	}
    }

  /*
   * Now go build the edge sets that correspond to each partition,
   * i.e., the set of edges that connect the nodes in each partition.
   */
  M_offset = 0;			/* init */
  for (p = 0; p < P; ++p)
    {
      part = QO_ENV_PARTITION (env, p);

      for (e = 0; e < E; ++e)
	{
	  QO_TERM *edge = QO_ENV_TERM (env, e);

	  if (bitset_subset
	      (&(QO_PARTITION_NODES (part)), &(QO_TERM_NODES (edge))))
	    {
	      bitset_add (&(QO_PARTITION_EDGES (part)), e);
	    }
	}
      /* alloc size check
       * 2: for signed max int. 2**30 is positive, 2**31 is negative
       * 2: for sizeof(QO_INFO *)
       */
      QO_ASSERT (env,
		 bitset_cardinality (&(QO_PARTITION_NODES (part))) <=
		 _WORDSIZE - 2 - 2);

      /* set the starting point the join_info vector that
       * correspond to each partition.
       */
      if (p > 0)
	{
	  QO_PARTITION_M_OFFSET (part) = M_offset;
	}
      join_info_size = QO_JOIN_INFO_SIZE (part);
      QO_ASSERT (env,
		 INT_MAX - M_offset * sizeof (QO_INFO *) >=
		 join_info_size * sizeof (QO_INFO *));
      M_offset += join_info_size;

      /* set the relative id of nodes in the partition
       */
      rel_idx = 0;		/* init */
      for (hi = bitset_iterate (&(QO_PARTITION_NODES (part)), &bi); hi != -1;
	   hi = bitset_next_member (&bi))
	{
	  QO_NODE_REL_IDX (QO_ENV_NODE (env, hi)) = rel_idx;
	  rel_idx++;
	}
    }

  env->npartitions = P;

  DEALLOCATE (env, buddy);
}

/*
 * qo_assign_eq_classes () -
 *   return:
 *   env(in):
 */
static void
qo_assign_eq_classes (QO_ENV * env)
{
  int i;
  QO_EQCLASS **eq_map;
  BITSET segs;

  bitset_init (&segs, env);
  eq_map = NALLOCATE (env, QO_EQCLASS *, env->nsegs);
  for (i = 0; i < env->nsegs; ++i)
    {
      eq_map[i] = NULL;
    }

  for (i = 0; i < env->nedges; i++)
    {
      QO_TERM *term;

      term = QO_ENV_TERM (env, i);
      if (QO_TERM_NOMINAL_SEG (term))
	{
	  bitset_union (&segs, &(QO_TERM_SEGS (term)));
	}
    }

  /*
   * Now examine each segment and see if it should be assigned to an
   * equivalence class.
   */
  for (i = 0; i < env->nsegs; ++i)
    {
      if (!BITSET_MEMBER (segs, i))
	{
	  continue;
	}

      if (eq_map[i] == NULL)
	{
	  QO_SEGMENT *root, *seg;
	  seg = QO_ENV_SEG (env, i);

	  /*
	   * Find the root of the tree in which this segment resides.
	   */
	  for (root = seg; QO_SEG_EQ_ROOT (root);
	       root = QO_SEG_EQ_ROOT (root))
	    {
	      ;
	    }
	  /*
	   * Assign a new EqClass to that root if one hasn't already
	   * been assigned.
	   */
	  if (eq_map[QO_SEG_IDX (root)] == NULL)
	    {
	      qo_eqclass_add ((eq_map[QO_SEG_IDX (root)] =
			       qo_eqclass_new (env)), root);
	    }
	  /*
	   * Now add the original segment to the same equivalence
	   * class.
	   */
	  if (root != seg)
	    {
	      qo_eqclass_add (eq_map[QO_SEG_IDX (root)], seg);
	    }
	  eq_map[i] = eq_map[QO_SEG_IDX (root)];
	}
    }

  bitset_delset (&segs);
  DEALLOCATE (env, eq_map);

  /*
   * Now squirrel away the eqclass info for each term so that we don't
   * have to keep recomputing it when searching the plan space.  Note
   * that this not really meaningful unless all of the segments in the
   * term are in the same equivalence class as the first one.  However,
   * since we're only supposed to use this information when examining
   * join or path terms, and since that condition holds for those
   * terms, this should be ok.
   */
  for (i = 0; i < env->nedges; i++)
    {
      QO_TERM *term = QO_ENV_TERM (env, i);
      QO_SEGMENT *seg = QO_TERM_NOMINAL_SEG (term);

      if (seg)
	{
	  QO_TERM_EQCLASS (term) = QO_SEG_EQCLASS (seg);
	}
      else if (QO_TERM_IS_FLAGED (term, QO_TERM_MERGEABLE_EDGE))
	{
	  QO_TERM_EQCLASS (term) = qo_eqclass_new (env);
	  QO_EQCLASS_TERM (QO_TERM_EQCLASS (term)) = term;
	}
      else
	{
	  QO_TERM_EQCLASS (term) = QO_UNORDERED;
	}
    }
}

/*
 * qo_env_dump () -
 *   return:
 *   env(in):
 *   f(in):
 */
static void
qo_env_dump (QO_ENV * env, FILE * f)
{
  int i;

  if (f == NULL)
    {
      f = stdout;
    }

  if (env->nsegs)
    {
      fprintf (f, "Join graph segments (f indicates final):\n");
      for (i = 0; i < env->nsegs; ++i)
	{
	  QO_SEGMENT *seg = QO_ENV_SEG (env, i);
	  int extra_info = 0;

	  fprintf (f, "seg[%d]: ", i);
	  qo_seg_fprint (seg, f);

	  PUT_FLAG (BITSET_MEMBER (env->final_segs, i), "f");
	  /*
	   * Put extra flags here.
	   */
	  fputs (extra_info ? ")\n" : "\n", f);
	}
    }

  if (env->nnodes)
    {
      fprintf (f, "Join graph nodes:\n");
      for (i = 0; i < env->nnodes; ++i)
	{
	  QO_NODE *node = QO_ENV_NODE (env, i);
	  fprintf (f, "node[%d]: ", i);
	  qo_node_dump (node, f);
	  fputs ("\n", f);
	}
    }

  if (env->neqclasses)
    {
      fprintf (f, "Join graph equivalence classes:\n");
      for (i = 0; i < env->neqclasses; ++i)
	{
	  fprintf (f, "eqclass[%d]: ", i);
	  qo_eqclass_dump (QO_ENV_EQCLASS (env, i), f);
	  fputs ("\n", f);
	}
    }

  /*
   * Notice that we blow off printing the edge structures themselves,
   * and just print the term that gives rise to the edge.  Also notice
   * the way edges and terms are separated: we don't reset the counter
   * for non-edge terms.
   */
  if (env->nedges)
    {
      fputs ("Join graph edges:\n", f);
      for (i = 0; i < env->nedges; ++i)
	{
	  fprintf (f, "term[%d]: ", i);
	  qo_term_dump (QO_ENV_TERM (env, i), f);
	  fputs ("\n", f);
	}
    }

  if (env->nterms - env->nedges)
    {
      fputs ("Join graph terms:\n", f);
      for (i = env->nedges; i < env->nterms; ++i)
	{
	  fprintf (f, "term[%d]: ", i);
	  qo_term_dump (QO_ENV_TERM (env, i), f);
	  fputs ("\n", f);
	}
    }

  if (env->nsubqueries)
    {
      fputs ("Join graph subqueries:\n", f);
      for (i = 0; i < env->nsubqueries; ++i)
	{
	  fprintf (f, "subquery[%d]: ", i);
	  qo_subquery_dump (env, &env->subqueries[i], f);
	  fputs ("\n", f);
	}
    }

  if (env->npartitions > 1)
    {
      fputs ("Join graph partitions:\n", f);
      for (i = 0; i < env->npartitions; ++i)
	{
	  fprintf (f, "partition[%d]: ", i);
	  qo_partition_dump (QO_ENV_PARTITION (env, i), f);
	  fputs ("\n", f);
	}
    }

  fflush (f);
}

/*
 * qo_node_clear () -
 *   return:
 *   env(in):
 *   idx(in):
 */
static void
qo_node_clear (QO_ENV * env, int idx)
{
  QO_NODE *node = QO_ENV_NODE (env, idx);

  QO_NODE_ENV (node) = env;
  QO_NODE_ENTITY_SPEC (node) = NULL;
  QO_NODE_PARTITION (node) = NULL;
  QO_NODE_OID_SEG (node) = NULL;
  QO_NODE_SELECTIVITY (node) = 1.0;
  QO_NODE_IDX (node) = idx;
  QO_NODE_INFO (node) = NULL;
  QO_NODE_NCARD (node) = 0;
  QO_NODE_TCARD (node) = 0;
  QO_NODE_NAME (node) = NULL;
  QO_NODE_INDEXES (node) = NULL;
  QO_NODE_USING_INDEX (node) = NULL;

  bitset_init (&(QO_NODE_EQCLASSES (node)), env);
  bitset_init (&(QO_NODE_SARGS (node)), env);
  bitset_init (&(QO_NODE_DEP_SET (node)), env);
  bitset_init (&(QO_NODE_SUBQUERIES (node)), env);
  bitset_init (&(QO_NODE_SEGS (node)), env);
  bitset_init (&(QO_NODE_OUTER_DEP_SET (node)), env);

  QO_NODE_HINT (node) = PT_HINT_NONE;
}

/*
 * qo_node_free () -
 *   return:
 *   node(in):
 */
static void
qo_node_free (QO_NODE * node)
{
  bitset_delset (&(QO_NODE_EQCLASSES (node)));
  bitset_delset (&(QO_NODE_SARGS (node)));
  bitset_delset (&(QO_NODE_DEP_SET (node)));
  bitset_delset (&(QO_NODE_SEGS (node)));
  bitset_delset (&(QO_NODE_SUBQUERIES (node)));
  bitset_delset (&(QO_NODE_OUTER_DEP_SET (node)));
  qo_free_class_info (QO_NODE_ENV (node), QO_NODE_INFO (node));
  if (QO_NODE_INDEXES (node))
    {
      qo_free_node_index_info (QO_NODE_ENV (node), QO_NODE_INDEXES (node));
    }
  DEALLOCATE (env, QO_NODE_USING_INDEX (node));
}

/*
 * qo_node_add_sarg () -
 *   return:
 *   node(in):
 *   sarg(in):
 */
static void
qo_node_add_sarg (QO_NODE * node, QO_TERM * sarg)
{
  double sel_limit;

  bitset_add (&(QO_NODE_SARGS (node)), QO_TERM_IDX (sarg));
  QO_NODE_SELECTIVITY (node) *= QO_TERM_SELECTIVITY (sarg);
  sel_limit =
    (QO_NODE_NCARD (node) == 0) ? 0 : (1.0 / (double) QO_NODE_NCARD (node));
  if (QO_NODE_SELECTIVITY (node) < sel_limit)
    {
      QO_NODE_SELECTIVITY (node) = sel_limit;
    }
}

/*
 * qo_node_fprint () -
 *   return:
 *   node(in):
 *   f(in):
 */
void
qo_node_fprint (QO_NODE * node, FILE * f)
{
  if (QO_NODE_NAME (node))
    {
      fprintf (f, "%s", QO_NODE_NAME (node));
    }
  fprintf (f, " node[%d]", QO_NODE_IDX (node));
}

/*
 * qo_node_dump () -
 *   return:
 *   node(in):
 *   f(in):
 */
static void
qo_node_dump (QO_NODE * node, FILE * f)
{
  int i, n = 1;
  const char *name;
  PT_NODE *entity;

  entity = QO_NODE_ENTITY_SPEC (node);

  if (QO_NODE_INFO (node))
    {
      n = QO_NODE_INFO_N (node);
      if (n > 1)
	{
	  fprintf (f, "(");	/* left paren */
	}
      for (i = 0; i < n; i++)
	{
	  name = QO_NODE_INFO (node)->info[i].name;
	  /* check for class OID reference spec
	   * for example: 'class x'
	   *   SELECT class_meth(class x, x.i) FROM x, class x
	   */
	  if (i == 0)
	    {			/* the first entity */
	      fprintf (f,
		       (entity->info.spec.meta_class == PT_META_CLASS
			? "class %s" : "%s"), (name ? name : "(anon)"));
	    }
	  else
	    {
	      fprintf (f,
		       (entity->info.spec.meta_class == PT_META_CLASS
			? ", class %s" : ", %s"), (name ? name : "(anon)"));
	    }
	}
      fprintf (f, "%s ", (n > 1 ? ")" : ""));	/* right paren */
    }
  name = QO_NODE_NAME (node);
  if (n == 1)
    {
      fprintf (f, "%s", (name ? name : "(unknown)"));
    }
  else
    {
      fprintf (f, "as %s", (name ? name : "(unknown)"));
    }

  if (entity->info.spec.range_var->alias_print)
    {
      fprintf (f, "(%s)", entity->info.spec.range_var->alias_print);
    }

  fprintf (f, "(%lu/%lu)", QO_NODE_NCARD (node), QO_NODE_TCARD (node));
  if (!bitset_is_empty (&(QO_NODE_SARGS (node))))
    {
      fputs (" (sargs ", f);
      bitset_print (&(QO_NODE_SARGS (node)), f);
      fputs (")", f);
    }
}

/*
 * qo_seg_clear () -
 *   return:
 *   env(in):
 *   idx(in):
 */
static void
qo_seg_clear (QO_ENV * env, int idx)
{
  QO_SEGMENT *seg = QO_ENV_SEG (env, idx);

  QO_SEG_ENV (seg) = env;
  QO_SEG_HEAD (seg) = NULL;
  QO_SEG_TAIL (seg) = NULL;
  QO_SEG_EQ_ROOT (seg) = NULL;
  QO_SEG_EQCLASS (seg) = NULL;
  QO_SEG_NAME (seg) = NULL;
  QO_SEG_INFO (seg) = NULL;
  QO_SEG_SET_VALUED (seg) = false;
  QO_SEG_CLASS_ATTR (seg) = false;
  QO_SEG_SHARED_ATTR (seg) = false;
  QO_SEG_IDX (seg) = idx;
  bitset_init (&(QO_SEG_INDEX_TERMS (seg)), env);
}

/*
 * qo_seg_free () -
 *   return:
 *   seg(in):
 */
static void
qo_seg_free (QO_SEGMENT * seg)
{
  if (QO_SEG_INFO (seg) != NULL)
    {
      qo_free_attr_info (QO_SEG_ENV (seg), QO_SEG_INFO (seg));
    }
  bitset_delset (&(QO_SEG_INDEX_TERMS (seg)));
}

/*
 * qo_seg_width () -
 *   return: size_t
 *   seg(in): A pointer to a QO_SEGMENT
 *
 * Note: Return the estimated width (in size_t units) of the indicated
 *	attribute.  This estimate will be required to estimate the
 *	size of intermediate results should they need to be
 *	materialized for e.g. sorting.
 */
int
qo_seg_width (QO_SEGMENT * seg)
{
  /*
   * This needs to consult the schema manager (or somebody) to
   * determine the type of the underlying attribute.  For set-valued
   * attributes, this is truly an estimate, since the size of the
   * attribute in any result tuple will be the product of the
   * cardinality of that particular set and the size of the underlying
   * element type.
   */
  int size;
  DB_DOMAIN *domain;

  domain = pt_node_to_db_domain (QO_ENV_PARSER (QO_SEG_ENV (seg)),
				 QO_SEG_PT_NODE (seg), NULL);
  if (domain)
    {
      domain = tp_domain_cache (domain);
    }
  else
    {
      /* guessing */
      return sizeof (int);
    }

  size = tp_domain_disk_size (domain);
  switch (domain->type->id)
    {
    case DB_TYPE_VARBIT:
    case DB_TYPE_VARCHAR:
    case DB_TYPE_VARNCHAR:
      /* do guessing for variable character type */
      size = size * (2 / 3);
      break;
    default:
      break;
    }
  return MAX ((int) sizeof (int), size);
  /* for backward compatibility, at least sizeof(long) */
}

/*
 * qo_seg_fprint () -
 *   return:
 *   seg(in):
 *   f(in):
 */
void
qo_seg_fprint (QO_SEGMENT * seg, FILE * f)
{
  fprintf (f, "%s[%d]", QO_SEG_NAME (seg), QO_NODE_IDX (QO_SEG_HEAD (seg)));
}

/*
 * qo_eqclass_new () -
 *   return:
 *   env(in):
 */
static QO_EQCLASS *
qo_eqclass_new (QO_ENV * env)
{
  QO_EQCLASS *eqclass;

  QO_ASSERT (env, env->neqclasses < env->Neqclasses);
  eqclass = QO_ENV_EQCLASS (env, env->neqclasses);

  QO_EQCLASS_ENV (eqclass) = env;
  QO_EQCLASS_IDX (eqclass) = env->neqclasses;
  QO_EQCLASS_TERM (eqclass) = NULL;

  bitset_init (&(QO_EQCLASS_SEGS (eqclass)), env);

  env->neqclasses++;

  return eqclass;
}

/*
 * qo_eqclass_free () -
 *   return:
 *   eqclass(in):
 */
static void
qo_eqclass_free (QO_EQCLASS * eqclass)
{
  bitset_delset (&(QO_EQCLASS_SEGS (eqclass)));
}

/*
 * qo_eqclass_add () -
 *   return:
 *   eqclass(in):
 *   seg(in):
 */
static void
qo_eqclass_add (QO_EQCLASS * eqclass, QO_SEGMENT * seg)
{
  bitset_add (&(QO_EQCLASS_SEGS (eqclass)), QO_SEG_IDX (seg));
  bitset_add (&(QO_NODE_EQCLASSES (QO_SEG_HEAD (seg))),
	      QO_EQCLASS_IDX (eqclass));
  QO_SEG_EQCLASS (seg) = eqclass;
}

/*
 * qo_eqclass_dump () -
 *   return:
 *   eqclass(in):
 *   f(in):
 */
static void
qo_eqclass_dump (QO_EQCLASS * eqclass, FILE * f)
{
  const char *prefix = "";
  int member;
  QO_ENV *env = QO_EQCLASS_ENV (eqclass);
  BITSET_ITERATOR bi;

  if (QO_EQCLASS_TERM (eqclass))
    {
      qo_term_fprint (QO_EQCLASS_TERM (eqclass), f);
    }
  else
    {
      for (member = bitset_iterate (&(QO_EQCLASS_SEGS (eqclass)), &bi);
	   member != -1; member = bitset_next_member (&bi))
	{
	  fputs (prefix, f);
	  qo_seg_fprint (QO_ENV_SEG (env, member), f);
	  prefix = " ";
	}
    }
}

/*
 * qo_term_clear () -
 *   return:
 *   env(in):
 *   idx(in):
 */
static void
qo_term_clear (QO_ENV * env, int idx)
{
  QO_TERM *term = QO_ENV_TERM (env, idx);

  QO_TERM_ENV (term) = env;
  QO_TERM_CLASS (term) = QO_TC_OTHER;
  QO_TERM_SELECTIVITY (term) = 1.0;
  QO_TERM_RANK (term) = 0;
  QO_TERM_PT_EXPR (term) = NULL;
  QO_TERM_LOCATION (term) = 0;
  QO_TERM_SEG (term) = NULL;
  QO_TERM_OID_SEG (term) = NULL;
  QO_TERM_HEAD (term) = NULL;
  QO_TERM_TAIL (term) = NULL;
  QO_TERM_EQCLASS (term) = QO_UNORDERED;
  QO_TERM_NOMINAL_SEG (term) = NULL;
  QO_TERM_IDX (term) = idx;

  QO_TERM_CAN_USE_INDEX (term) = 0;
  QO_TERM_INDEX_SEG (term, 0) = NULL;
  QO_TERM_INDEX_SEG (term, 1) = NULL;
  QO_TERM_JOIN_TYPE (term) = NO_JOIN;

  bitset_init (&(QO_TERM_NODES (term)), env);
  bitset_init (&(QO_TERM_SEGS (term)), env);
  bitset_init (&(QO_TERM_SUBQUERIES (term)), env);

  QO_TERM_FLAG (term) = 0;
}

/*
 * qo_equivalence () -
 *   return:
 *   sega(in):
 *   segb(in):
 */
static void
qo_equivalence (QO_SEGMENT * sega, QO_SEGMENT * segb)
{

  while (QO_SEG_EQ_ROOT (sega))
    {
      sega = QO_SEG_EQ_ROOT (sega);
    }
  while (QO_SEG_EQ_ROOT (segb))
    {
      segb = QO_SEG_EQ_ROOT (segb);
    }

  if (sega != segb)
    {
      QO_SEG_EQ_ROOT (sega) = segb;
    }
}

/*
 * qo_eqclass_wrt () -
 *   return:
 *   eqclass(in):
 *   nodeset(in):
 */
static QO_SEGMENT *
qo_eqclass_wrt (QO_EQCLASS * eqclass, BITSET * nodeset)
{
  int member;
  BITSET_ITERATOR si;
  QO_SEGMENT *result = NULL;

  for (member = bitset_iterate (&(QO_EQCLASS_SEGS (eqclass)), &si);
       member != -1; member = bitset_next_member (&si))
    {
      QO_SEGMENT *seg = QO_ENV_SEG (QO_EQCLASS_ENV (eqclass), member);
      if (BITSET_MEMBER (*nodeset, QO_NODE_IDX (QO_SEG_HEAD (seg))))
	{
	  result = seg;
	  break;
	}
    }

  QO_ASSERT (eqclass->env, result != NULL);
  return result;
}

/*
 * qo_eqclass_fprint_wrt () -
 *   return:
 *   eqclass(in):
 *   nodeset(in):
 *   f(in):
 */
void
qo_eqclass_fprint_wrt (QO_EQCLASS * eqclass, BITSET * nodeset, FILE * f)
{
  if (eqclass == QO_UNORDERED)
    {
      fputs ("UNORDERED", f);
    }
  else if (bitset_is_empty (&(QO_EQCLASS_SEGS (eqclass))))
    {
      /*
       * This is a phony eqclass created for a complex merge join.
       * Just fabricate some text that will let us know where it came
       * from...
       */
      fprintf (f, "phony (term[%d])",
	       QO_TERM_IDX (QO_EQCLASS_TERM (eqclass)));
    }
  else
    {
      qo_seg_fprint (qo_eqclass_wrt (eqclass, nodeset), f);
    }
}

/*
 * qo_term_free () -
 *   return:
 *   term(in):
 */
static void
qo_term_free (QO_TERM * term)
{
  /*
   * Free the expr alloced by this term
   */
  if (QO_TERM_IS_FLAGED (term, QO_TERM_COPY_PT_EXPR))
    {
      parser_free_tree (QO_ENV_PARSER (QO_TERM_ENV (term)),
			QO_TERM_PT_EXPR (term));
    }
  bitset_delset (&(QO_TERM_NODES (term)));
  bitset_delset (&(QO_TERM_SEGS (term)));
  bitset_delset (&(QO_TERM_SUBQUERIES (term)));
}

/*
 * qo_term_fprint () -
 *   return:
 *   term(in):
 *   f(in):
 */
void
qo_term_fprint (QO_TERM * term, FILE * f)
{
  QO_TERMCLASS tc;

  if (term)
    {
      switch (tc = QO_TERM_CLASS (term))
	{
	case QO_TC_PATH:
	  qo_node_fprint (QO_TERM_HEAD (term), f);
	  if (!QO_TERM_SEG (term) || !QO_SEG_NAME (QO_TERM_SEG (term)))
	    {
	      fprintf (f, " () -> ");
	    }
	  else
	    {
	      fprintf (f, " %s -> ", QO_SEG_NAME (QO_TERM_SEG (term)));
	    }
	  qo_node_fprint (QO_TERM_TAIL (term), f);
	  break;

	case QO_TC_DEP_LINK:
	  fprintf (f, "table(");
	  bitset_print (&(QO_NODE_DEP_SET (QO_TERM_TAIL (term))), f);
	  fprintf (f, ") -> ");
	  qo_node_fprint (QO_TERM_TAIL (term), f);
	  break;

	case QO_TC_DEP_JOIN:
	  qo_node_fprint (QO_TERM_HEAD (term), f);
	  fprintf (f, " <dj> ");
	  qo_node_fprint (QO_TERM_TAIL (term), f);
	  break;

	default:
	  fprintf (f, "term[%d]", QO_TERM_IDX (term));
	  break;
	}
    }
  else
    {
      fprintf (f, "none");
    }
}

/*
 * qo_termset_fprint () -
 *   return:
 *   env(in):
 *   terms(in):
 *   f(in):
 */
void
qo_termset_fprint (QO_ENV * env, BITSET * terms, FILE * f)
{
  int tx;
  BITSET_ITERATOR si;
  const char *prefix = "";

  for (tx = bitset_iterate (terms, &si);
       tx != -1; tx = bitset_next_member (&si))
    {
      fputs (prefix, f);
      qo_term_fprint (QO_ENV_TERM (env, tx), f);
      prefix = " AND ";
    }
}

/*
 * qo_term_dump () -
 *   return:
 *   term(in):
 *   f(in):
 */
static void
qo_term_dump (QO_TERM * term, FILE * f)
{
  QO_TERMCLASS tc;

  switch (tc = QO_TERM_CLASS (term))
    {
    case QO_TC_PATH:
      qo_node_fprint (QO_TERM_HEAD (term), f);
      if (!QO_TERM_SEG (term) || !QO_SEG_NAME (QO_TERM_SEG (term)))
	{
	  fprintf (f, " () -> ");
	}
      else
	{
	  fprintf (f, " %s -> ", QO_SEG_NAME (QO_TERM_SEG (term)));
	}
      qo_node_fprint (QO_TERM_TAIL (term), f);
      break;

    case QO_TC_DEP_LINK:
      fprintf (f, "table(");
      bitset_print (&(QO_NODE_DEP_SET (QO_TERM_TAIL (term))), f);
      fprintf (f, ") -> ");
      qo_node_fprint (QO_TERM_TAIL (term), f);
      break;

    case QO_TC_DEP_JOIN:
      qo_node_fprint (QO_TERM_HEAD (term), f);
      fprintf (f, " <dj> ");
      qo_node_fprint (QO_TERM_TAIL (term), f);
      break;

    case QO_TC_DUMMY_JOIN:
      if (QO_TERM_PT_EXPR (term))
	{			/* may be transitive dummy join term */
	  fprintf (f, "%s",
		   parser_print_tree (QO_ENV_PARSER (QO_TERM_ENV (term)),
				      QO_TERM_PT_EXPR (term)));
	}
      else
	{
	  qo_node_fprint (QO_TERM_HEAD (term), f);
	  fprintf (f, ", ");
	  qo_node_fprint (QO_TERM_TAIL (term), f);
	}
      break;

    default:
      {
	PARSER_CONTEXT *parser = QO_ENV_PARSER (QO_TERM_ENV (term));
	PT_PRINT_VALUE_FUNC saved_func = parser->print_db_value;
	/* in order to print auto parameterized values */
	parser->print_db_value = pt_print_node_value;
	fprintf (f, "%s", parser_print_tree (parser, QO_TERM_PT_EXPR (term)));
	parser->print_db_value = saved_func;
      }
      break;
    }
  fprintf (f, " (sel %g)", QO_TERM_SELECTIVITY (term));

  if (QO_TERM_RANK (term) > 1)
    {
      fprintf (f, " (rank %d)", QO_TERM_RANK (term));
    }

  switch (QO_TERM_CLASS (term))
    {
    case QO_TC_PATH:
      fprintf (f, " (path term)");
      break;
    case QO_TC_JOIN:
      fprintf (f, " (join term)");
      break;
    case QO_TC_SARG:
      fprintf (f, " (sarg term)");
      break;
    case QO_TC_OTHER:
      {
	PT_NODE *conj;

	conj = QO_TERM_PT_EXPR (term);
	if (conj && conj->node_type == PT_VALUE
	    && conj->info.value.location == 0)
	  {
	    /* is an always-false WHERE condition */
	    fprintf (f, " (always-false term)");
	  }
	else
	  {
	    fprintf (f, " (other term)");
	  }
      }
      break;
    case QO_TC_DEP_LINK:
      fprintf (f, " (dep term)");
      break;
    case QO_TC_DEP_JOIN:
      fprintf (f, " (dep-join term)");
      break;
    case QO_TC_DURING_JOIN:
      fprintf (f, " (during join term)");
      break;
    case QO_TC_AFTER_JOIN:
      fprintf (f, " (after join term)");
      break;
    case QO_TC_TOTALLY_AFTER_JOIN:
      fprintf (f, " (instnum term)");
      break;
    case QO_TC_DUMMY_JOIN:
      fprintf (f, " (dummy join term)");
      break;
    default:
      break;
    }

  if (QO_TERM_IS_FLAGED (term, QO_TERM_MERGEABLE_EDGE))
    {
      fputs (" (mergeable)", f);
    }

  switch (QO_TERM_JOIN_TYPE (term))
    {
    case NO_JOIN:
      fputs (" (not-join eligible)", f);
      break;

    case JOIN_INNER:
      fputs (" (inner-join)", f);
      break;

    case JOIN_LEFT:
      fputs (" (left-join)", f);
      break;

    case JOIN_RIGHT:
      fputs (" (right-join)", f);
      break;

    case JOIN_OUTER:
      fputs (" (outer-join)", f);
      break;

    default:
      break;
    }

  if (QO_TERM_CAN_USE_INDEX (term))
    {
      int i;
      fputs (" (indexable", f);
      for (i = 0; i < QO_TERM_CAN_USE_INDEX (term); i++)
	{
	  fputs (" ", f);
	  qo_seg_fprint (QO_TERM_INDEX_SEG (term, i), f);
	}
      fputs (")", f);
    }

  fprintf (f, " (loc %d)", QO_TERM_LOCATION (term));
}

/*
 * qo_subquery_dump () -
 *   return:
 *   env(in):
 *   subq(in):
 *   f(in):
 */
static void
qo_subquery_dump (QO_ENV * env, QO_SUBQUERY * subq, FILE * f)
{
  int i;
  BITSET_ITERATOR bi;
  const char *separator;

  fprintf (f, "%p", (void *) subq->node);

  separator = NULL;
  fputs (" {", f);
  for (i = bitset_iterate (&(subq->segs), &bi);
       i != -1; i = bitset_next_member (&bi))
    {
      if (separator)
	{
	  fputs (separator, f);
	}
      qo_seg_fprint (QO_ENV_SEG (env, i), f);
      separator = " ";
    }
  fputs ("}", f);

  separator = "";
  fputs (" {", f);
  for (i = bitset_iterate (&(subq->nodes), &bi);
       i != -1; i = bitset_next_member (&bi))
    {
      fprintf (f, "%snode[%d]", separator, i);
      separator = " ";
    }
  fputs ("}", f);

  fputs (" (from term(s)", f);
  for (i = bitset_iterate (&(subq->terms), &bi);
       i != -1; i = bitset_next_member (&bi))
    {
      fprintf (f, " %d", i);
    }
  fputs (")", f);
}

/*
 * qo_subquery_free () -
 *   return:
 *   subq(in):
 */
static void
qo_subquery_free (QO_SUBQUERY * subq)
{
  bitset_delset (&(subq->segs));
  bitset_delset (&(subq->nodes));
  bitset_delset (&(subq->terms));
}

/*
 * qo_partition_init () -
 *   return:
 *   env(in):
 *   part(in):
 *   n(in):
 */
static void
qo_partition_init (QO_ENV * env, QO_PARTITION * part, int n)
{
  bitset_init (&(QO_PARTITION_NODES (part)), env);
  bitset_init (&(QO_PARTITION_EDGES (part)), env);
  bitset_init (&(QO_PARTITION_DEPENDENCIES (part)), env);
  QO_PARTITION_M_OFFSET (part) = 0;
  QO_PARTITION_PLAN (part) = NULL;
  QO_PARTITION_IDX (part) = n;
}

/*
 * qo_partition_free () -
 *   return:
 *   part(in):
 */
static void
qo_partition_free (QO_PARTITION * part)
{
  bitset_delset (&(QO_PARTITION_NODES (part)));
  bitset_delset (&(QO_PARTITION_EDGES (part)));
  bitset_delset (&(QO_PARTITION_DEPENDENCIES (part)));

#if 0
  /*
   * Do *not* free the plan here; it already had its ref count bumped
   * down during combine_partitions(), and it will be (has been)
   * collected by the call to qo_plan_discard() that freed the
   * top-level plan.  What we have here is a dangling pointer.
   */
  qo_plan_del_ref (QO_PARTITION_PLAN (part));
#else
  QO_PARTITION_PLAN (part) = NULL;
#endif
}


/*
 * qo_partition_dump () -
 *   return:
 *   part(in):
 *   f(in):
 */
static void
qo_partition_dump (QO_PARTITION * part, FILE * f)
{
  fputs ("(nodes ", f);
  bitset_print (&(QO_PARTITION_NODES (part)), f);
  fputs (") (edges ", f);
  bitset_print (&(QO_PARTITION_EDGES (part)), f);
  fputs (") (dependencies ", f);
  bitset_print (&(QO_PARTITION_DEPENDENCIES (part)), f);
  fputs (")", f);
}

/*
 * qo_print_stats () -
 *   return:
 *   f(in):
 */
void
qo_print_stats (FILE * f)
{
  fputs ("\n", f);
  qo_info_stats (f);
  qo_plans_stats (f);
  set_stats (f);
}

/*
 * qo_seg_nodes () - Return a bitset of node ids produced from the heads
 *		     of all of the segments in segset
 *   return:
 *   env(in): The environment in which these segment and node ids make sense
 *   segset(in): A bitset of segment ids
 *   result(out): A bitset of node ids (OUTPUT PARAMETER)
 */
static void
qo_seg_nodes (QO_ENV * env, BITSET * segset, BITSET * result)
{
  BITSET_ITERATOR si;
  int i;

  BITSET_CLEAR (*result);
  for (i = bitset_iterate (segset, &si); i != -1;
       i = bitset_next_member (&si))
    {
      bitset_add (result, QO_NODE_IDX (QO_SEG_HEAD (QO_ENV_SEG (env, i))));
    }
}
