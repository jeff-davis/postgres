/*-------------------------------------------------------------------------
 * import_stats.c
 *
 *	  PostgreSQL statistics import
 *
 * Code supporting the direct importation of relation statistics, similar to
 * what is done by the ANALYZE command.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *       src/backend/statistics/import_stats.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/indexing.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_operator.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "statistics/statistics.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * Names of parameters found in the functions pg_set_relation_stats and
 * pg_set_attribute_stats
 */
const char *relation_name = "relation";
const char *relpages_name = "relpages";
const char *reltuples_name = "reltuples";
const char *relallvisible_name = "relallvisible";
const char *attname_name = "attname";
const char *inherited_name = "inherited";
const char *null_frac_name = "null_frac";
const char *avg_width_name = "avg_width";
const char *n_distinct_name = "n_distinct";
const char *mc_vals_name = "most_common_vals";
const char *mc_freqs_name = "most_common_freqs";
const char *histogram_bounds_name = "histogram_bounds";
const char *correlation_name = "correlation";
const char *mc_elems_name = "most_common_elems";
const char *mc_elem_freqs_name = "most_common_elem_freqs";
const char *elem_count_hist_name = "elem_count_histogram";
const char *range_length_hist_name = "range_length_histogram";
const char *range_empty_frac_name = "range_empty_frac";
const char *range_bounds_hist_name = "range_bounds_histogram";

/*
 * A role has privileges to set statistics on the relation if any of the
 * following are true:
 *   - the role owns the current database and the relation is not shared
 *   - the role has the MAINTAIN privilege on the relation
 */
static bool
check_can_update_stats(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	Form_pg_class reltuple = rel->rd_rel;

	return ((object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId())
			 && !reltuple->relisshared) ||
			pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) == ACLCHECK_OK);
}

/*
 * Internal function for modifying statistics for a relation.
 */
static bool
relation_statistics_update(Oid reloid, int version, int32 relpages,
						   float4 reltuples, int32 relallvisible, int elevel)
{
	Relation		relation;
	Relation		crel;
	HeapTuple		ctup;
	Form_pg_class	pgcform;

	if (relpages < -1)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relpages cannot be < -1")));
		return false;
	}

	if (reltuples < -1.0)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("reltuples cannot be < -1.0")));
		return false;
	}

	if (relallvisible < -1)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relallvisible cannot be < -1")));
		return false;
	}

	/*
	 * Open relation with ShareUpdateExclusiveLock, consistent with
	 * ANALYZE. Only needed for permission check and then we close it (but
	 * retain the lock).
	 */
	relation = try_table_open(reloid, ShareUpdateExclusiveLock);

	if (!relation)
	{
		elog(elevel, "could not open relation with OID %u", reloid);
		return false;
	}

	if (!check_can_update_stats(relation))
	{
		ereport(elevel,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for relation %s",
						RelationGetRelationName(relation))));
		table_close(relation, NoLock);
		return false;
	}

	table_close(relation, NoLock);

	/*
	 * Take RowExclusiveLock on pg_class, consistent with
	 * vac_update_relstats().
	 */
	crel = table_open(RelationRelationId, RowExclusiveLock);

	ctup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(reloid));
	if (!HeapTupleIsValid(ctup))
	{
		ereport(elevel,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("pg_class entry for relid %u not found", reloid)));
		return false;
	}

	pgcform = (Form_pg_class) GETSTRUCT(ctup);

	/* only update pg_class if there is a meaningful change */
	if (pgcform->relpages != relpages ||
		pgcform->reltuples != reltuples ||
		pgcform->relallvisible != relallvisible)
	{
		int			cols[3] = {
			Anum_pg_class_relpages,
			Anum_pg_class_reltuples,
			Anum_pg_class_relallvisible,
		};
		Datum		values[3] = {
			Int32GetDatum(relpages),
			Float4GetDatum(reltuples),
			Int32GetDatum(relallvisible),
		};
		bool		nulls[3] = {false, false, false};

		TupleDesc	tupdesc = RelationGetDescr(crel);
		HeapTuple	newtup;

		CatalogIndexState indstate = CatalogOpenIndexes(crel);

		newtup = heap_modify_tuple_by_cols(ctup, tupdesc, 3, cols, values,
										   nulls);

		CatalogTupleUpdateWithInfo(crel, &newtup->t_self, newtup, indstate);
		heap_freetuple(newtup);
		CatalogCloseIndexes(indstate);
	}

	/* release the lock, consistent with vac_update_relstats() */
	table_close(crel, RowExclusiveLock);

	PG_RETURN_BOOL(true);
}

/*
 * Set statistics for a given pg_class entry.
 *
 * Use a transactional update, and assume statistics come from the current
 * server version.
 *
 * Not intended for bulk import of statistics from older versions.
 */
Datum
pg_set_relation_stats(PG_FUNCTION_ARGS)
{
	Oid			reloid;
	int			version = PG_VERSION_NUM;
	int			elevel = ERROR;
	int32		relpages;
	float		reltuples;
	int32		relallvisible;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be NULL")));
	else
		reloid = PG_GETARG_OID(0);

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relpages cannot be NULL")));
	else
		relpages = PG_GETARG_INT32(1);

	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("reltuples cannot be NULL")));
	else
		reltuples = PG_GETARG_FLOAT4(2);

	if (PG_ARGISNULL(3))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relallvisible cannot be NULL")));
	else
		relallvisible = PG_GETARG_INT32(3);


	relation_statistics_update(reloid, version, relpages, reltuples,
							   relallvisible, elevel);

	PG_RETURN_VOID();
}

/*
 * Test if the type is a scalar for MCELEM purposes
 */
static bool
type_is_scalar(Oid typid)
{
	HeapTuple	tp;
	bool		result = false;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);

		result = (!OidIsValid(typtup->typanalyze));
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * If this relation is an index and that index has expressions in it, and
 * the attnum specified is known to be an expression, then we must walk
 * the list attributes up to the specified attnum to get the right
 * expression.
 */
static Node *
get_attr_expr(Relation rel, int attnum)
{
	if ((rel->rd_rel->relkind == RELKIND_INDEX
		 || (rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX))
		&& (rel->rd_indexprs != NIL)
		&& (rel->rd_index->indkey.values[attnum - 1] == 0))
	{
		ListCell   *indexpr_item = list_head(rel->rd_indexprs);

		for (int i = 0; i < attnum - 1; i++)
			if (rel->rd_index->indkey.values[i] == 0)
				indexpr_item = lnext(rel->rd_indexprs, indexpr_item);

		if (indexpr_item == NULL)	/* shouldn't happen */
			elog(ERROR, "too few entries in indexprs list");

		return (Node *) lfirst(indexpr_item);
	}
	return NULL;
}

/*
 * Fetch datatype information, this is needed to derive the proper staopN
 * and stacollN values.
 *
 */
static TypeCacheEntry *
get_attr_stat_type(Relation rel, Name attname, int elevel,
				   int16 *attnum, int32 *typmod, Oid *typcoll)
{
	Oid			relid = RelationGetRelid(rel);
	Form_pg_attribute attr;
	HeapTuple	atup;
	Oid			typid;
	Node	   *expr;

	atup = SearchSysCache2(ATTNAME, ObjectIdGetDatum(relid),
						   NameGetDatum(attname));

	/* Attribute not found */
	if (!HeapTupleIsValid(atup))
	{
		ereport(elevel,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("Relation %s has no attname %s",
						RelationGetRelationName(rel),
						NameStr(*attname))));
		return NULL;
	}

	attr = (Form_pg_attribute) GETSTRUCT(atup);
	if (attr->attisdropped)
	{
		ereport(elevel,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("Relation %s attname %s is dropped",
						RelationGetRelationName(rel),
						NameStr(*attname))));
		return NULL;
	}
	*attnum = attr->attnum;

	expr = get_attr_expr(rel, attr->attnum);

	if (expr == NULL)
	{
		/* regular attribute */
		typid = attr->atttypid;
		*typmod = attr->atttypmod;
		*typcoll = attr->attcollation;
	}
	else
	{
		typid = exprType(expr);
		*typmod = exprTypmod(expr);

		/*
		 * If a collation has been specified for the index column, use that in
		 * preference to anything else; but if not, fall back to whatever we
		 * can get from the expression.
		 */
		if (OidIsValid(attr->attcollation))
			*typcoll = attr->attcollation;
		else
			*typcoll = exprCollation(expr);
	}
	ReleaseSysCache(atup);

	/* if it's a multirange, step down to the range type */
	if (type_is_multirange(typid))
		typid = get_multirange_range(typid);

	return lookup_type_cache(typid,
							 TYPECACHE_LT_OPR | TYPECACHE_EQ_OPR);
}

/*
 * Perform the cast of a known TextDatum into the type specified.
 *
 * If no errors are found, ok is set to true.
 *
 * Otherwise, set ok to false, capture the error found, and re-throw at the
 * level specified by elevel.
 */
static Datum
cast_stavalues(FmgrInfo *flinfo, Datum d, Oid typid, int32 typmod, 
			   int elevel, bool *ok)
{
	LOCAL_FCINFO(fcinfo, 8);
	char	   *s;
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	escontext.details_wanted = true;

	s = TextDatumGetCString(d);

	InitFunctionCallInfoData(*fcinfo, flinfo, 3, InvalidOid,
							 (Node *) &escontext, NULL);

	fcinfo->args[0].value = CStringGetDatum(s);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typid);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	if (SOFT_ERROR_OCCURRED(&escontext))
	{
		if (elevel != ERROR)
			escontext.error_data->elevel = elevel;
		ThrowErrorData(escontext.error_data);
		*ok = false;
	}
	else
		*ok = true;

	pfree(s);

	return result;
}


/*
 * Check array for any NULLs, and optionally for one-dimensionality.
 *
 * Report any failures at the level of elevel.
 */
static bool
array_check(Datum datum, int one_dim, const char *statname, int elevel)
{
	ArrayType  *arr = DatumGetArrayTypeP(datum);
	int16		elmlen;
	char		elmalign;
	bool		elembyval;
	Datum	   *values;
	bool	   *nulls;
	int			nelems;

	if (one_dim && (arr->ndim != 1))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cannot be a multidimensional array", statname)));
		return false;
	}

	get_typlenbyvalalign(ARR_ELEMTYPE(arr), &elmlen, &elembyval, &elmalign);

	deconstruct_array(arr, ARR_ELEMTYPE(arr), elmlen, elembyval, elmalign,
					  &values, &nulls, &nelems);

	for (int i = 0; i < nelems; i++)
		if (nulls[i])
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s array cannot contain NULL values", statname)));
			return false;
		}
	return true;
}

/*
 * Update the pg_statistic record.
 */
static void
update_pg_statistic(Datum values[], bool nulls[])
{
	Relation	sd = table_open(StatisticRelationId, RowExclusiveLock);
	CatalogIndexState indstate = CatalogOpenIndexes(sd);
	HeapTuple	oldtup;
	HeapTuple	stup;

	/* Is there already a pg_statistic tuple for this attribute? */
	oldtup = SearchSysCache3(STATRELATTINH,
							 values[Anum_pg_statistic_starelid - 1],
							 values[Anum_pg_statistic_staattnum - 1],
							 values[Anum_pg_statistic_stainherit - 1]);

	if (HeapTupleIsValid(oldtup))
	{
		/* Yes, replace it */
		bool		replaces[Natts_pg_statistic];

		for (int i = 0; i < Natts_pg_statistic; i++)
			replaces[i] = true;

		stup = heap_modify_tuple(oldtup, RelationGetDescr(sd),
								 values, nulls, replaces);
		ReleaseSysCache(oldtup);
		CatalogTupleUpdateWithInfo(sd, &stup->t_self, stup, indstate);
	}
	else
	{
		/* No, insert new tuple */
		stup = heap_form_tuple(RelationGetDescr(sd), values, nulls);
		CatalogTupleInsertWithInfo(sd, stup, indstate);
	}

	heap_freetuple(stup);
	CatalogCloseIndexes(indstate);
	table_close(sd, NoLock);
}

/*
 * Insert or Update Attribute Statistics
 */
static bool
attribute_statistics_update(Datum relation_datum, bool relation_isnull,
							Datum attname_datum, bool attname_isnull,
							Datum inherited_datum, bool inherited_isnull,
							Datum version_datum, bool version_isnull,
							Datum null_frac_datum, bool null_frac_isnull,
							Datum avg_width_datum, bool avg_width_isnull,
							Datum n_distinct_datum, bool n_distinct_isnull,
							Datum mc_vals_datum, bool mc_vals_isnull,
							Datum mc_freqs_datum, bool mc_freqs_isnull, 
							Datum histogram_bounds_datum, bool histogram_bounds_isnull,
							Datum correlation_datum, bool correlation_isnull,
							Datum mc_elems_datum, bool mc_elems_isnull,
							Datum mc_elem_freqs_datum, bool mc_elem_freqs_isnull,
							Datum elem_count_hist_datum, bool elem_count_hist_isnull,
							Datum range_length_hist_datum, bool range_length_hist_isnull,
							Datum range_empty_frac_datum, bool range_empty_frac_isnull,
							Datum range_bounds_hist_datum, bool range_bounds_hist_isnull,
							bool raise_errors)
{
	int			elevel = (raise_errors) ? ERROR : WARNING;

	Oid			relation;
	Name		attname;
	int			version;

	Relation	rel;

	TypeCacheEntry *typcache;
	TypeCacheEntry *elemtypcache = NULL;

	int16		attnum;
	int32		typmod;
	Oid			typcoll;

	FmgrInfo	finfo;

	int			stakind_count;

	Datum		values[Natts_pg_statistic];
	bool		nulls[Natts_pg_statistic];

	/*
	 * The statkind index, we have only STATISTIC_NUM_SLOTS to hold these stats
	 */
	int			stakindidx = 0;

	/*
	 * Initialize output tuple.
	 *
	 * All non-repeating attributes should be NOT NULL. Only values for unused
	 * statistics slots, and certain stakind-specific values for stanumbersN
	 * and stavaluesN will ever be set NULL.
	 */
	for (int i = 0; i < Natts_pg_statistic; i++)
	{
		values[i] = (Datum) 0;
		nulls[i] = false;
	}

	/*
	 * Some parameters are "required" in that nothing can happen if any of
	 * them are NULL.
	 */
	if (relation_isnull)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cannot be NULL", relation_name)));
		return false;
	}
	relation = DatumGetObjectId(relation_datum);

	if (attname_isnull)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cannot be NULL", attname_name)));
		return false;
	}
	attname = DatumGetName(attname_datum);

	if (inherited_isnull)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cannot be NULL", inherited_name)));
		return false;
	}

	/*
	 * NULL version means assume current server version
	 */
	version = (version_isnull) ? PG_VERSION_NUM : DatumGetInt32(version_datum);
	if (version < 90200)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Cannot export statistics prior to version 9.2")));
		return false;
	}

	if (null_frac_isnull)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cannot be NULL", null_frac_name)));
		return false;
	}

	if (avg_width_isnull)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cannot be NULL", avg_width_name)));
		return false;
	}

	if (n_distinct_isnull)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cannot be NULL", n_distinct_name)));
		return false;
	}

	/*
	 * Some parameters are linked, should both be NULL or NOT NULL.
	 * Disagreement means that the statistic pair will fail so the 
	 * NOT NULL one must be abandoned (set NULL) after an
	 * ERROR/WARNING. By ensuring that the values are aligned it is 
	 * possible to use one as a proxy for the other later.
	 */

	/*
	 * STATISTIC_KIND_MCV = mc_vals + mc_freqs
	 */
	if (mc_vals_isnull != mc_freqs_isnull)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cannot be NULL when %s is NOT NULL",
						mc_vals_isnull ? mc_vals_name : mc_freqs_name,
						!mc_vals_isnull ? mc_vals_name : mc_freqs_name)));

		mc_vals_isnull = true;
		mc_freqs_isnull = true;
	}

	/*
	 * STATISTIC_KIND_MCELEM = mc_elems + mc_elem_freqs
	 */
	if (mc_elems_isnull != mc_elem_freqs_isnull)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cannot be NULL when %s is NOT NULL",
						mc_elems_isnull ? mc_elems_name : mc_elem_freqs_name,
						!mc_elems_isnull ? mc_elems_name : mc_elem_freqs_name)));
		mc_elems_isnull = true;
		mc_elem_freqs_isnull = true;
	}

	/*
	 * STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM =
	 * range_length_histogram + range_empty_frac
	 */
	else if (range_length_hist_isnull != range_empty_frac_isnull)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cannot be NULL when %s is NOT NULL",
						range_length_hist_isnull ?
						range_length_hist_name : range_empty_frac_name,
						!range_length_hist_isnull ?
						range_length_hist_name : range_empty_frac_name)));
		range_length_hist_isnull = true;
		range_empty_frac_isnull = true;
	}

	/*
	 * If a caller specifies more stakind-stats than we have slots to store
	 * them, reject them all.
	 */
	stakind_count = (int) !mc_vals_isnull +
		(int) !mc_elems_isnull +
		(int) (!range_length_hist_isnull) +
		(int) !histogram_bounds_isnull +
		(int) !correlation_isnull +
		(int) !elem_count_hist_isnull +
		(int) !range_bounds_hist_isnull;

	if (stakind_count > STATISTIC_NUM_SLOTS)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("imported statistics must have a maximum of %d slots "
						"but %d given",
						STATISTIC_NUM_SLOTS, stakind_count)));
		return false;
	}

	rel = try_relation_open(relation, ShareUpdateExclusiveLock);

	if (rel == NULL)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Parameter relation OID %u is invalid", relation)));
		return false;
	}

	/*
	 * Many of the values that are set for a particular stakind are entirely
	 * derived from the attribute itself, or it's expression.
	 */
	typcache = get_attr_stat_type(rel, attname, elevel, &attnum, &typmod, &typcoll);
	if (typcache == NULL)
	{
		relation_close(rel, NoLock);
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unexpected typecache error")));
		return false;
	}

	/*
	 * Derive element type if we have stat kinds that need it.
	 *
	 * This duplicates some type-specific logic found in various typanalyze
	 * functions which are called from vacuum's examine_attribute(), but using
	 * that directly has proven awkward.
	 */
	if (!mc_elems_isnull || !elem_count_hist_isnull)
	{
		Oid			elemtypid;

		if (typcache->type_id == TSVECTOROID)
		{
			/*
			 * tsvectors always have a text oid base type and default
			 * collation
			 */
			elemtypid = TEXTOID;
			typcoll = DEFAULT_COLLATION_OID;
		}
		else if (typcache->typtype == TYPTYPE_RANGE)
			elemtypid = get_range_subtype(typcache->type_id);
		else
			elemtypid = get_base_element_type(typcache->type_id);

		/* not finding a basetype means we already had it */
		if (elemtypid == InvalidOid)
			elemtypid = typcache->type_id;

		/* The stats need the eq_opr, any validation would need the lt_opr */
		elemtypcache = lookup_type_cache(elemtypid, TYPECACHE_EQ_OPR);
		if (elemtypcache == NULL)
		{
			/* warn and ignore any stats that can't be fulfilled */
			if (!mc_elems_isnull)
			{
				ereport(elevel,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("%s cannot accept %s stats, ignored",
								NameStr(*attname),
								mc_elems_name)));
				mc_elems_isnull = true;
				mc_elem_freqs_isnull = true;
			}

			if (!elem_count_hist_isnull)
			{
				ereport(elevel,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("%s cannot accept %s stats, ignored",
								NameStr(*attname),
								elem_count_hist_name)));
				elem_count_hist_isnull = true;
			}
		}
	}

	/*
	 * histogram_bounds and correlation must have a type < operator
	 */
	if (typcache->lt_opr == InvalidOid)
	{
		if (!histogram_bounds_isnull)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Relation %s attname %s cannot "
							"have stats of type %s, ignored.",
							RelationGetRelationName(rel),
							NameStr(*attname),
							histogram_bounds_name)));
			histogram_bounds_isnull = true;
		}

		if (!correlation_isnull)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Relation %s attname %s cannot "
							"have stats of type %s, ignored.",
							RelationGetRelationName(rel),
							NameStr(*attname),
							correlation_name)));
			correlation_isnull = true;
		}
	}

	/*
	 * Scalar types can't have most_common_elems, most_common_elem_freqs, or
	 * element_count_histogram
	 */
	if (type_is_scalar(typcache->type_id))
	{
		/* warn and ignore any stats that can't be fulfilled */
		if (!mc_elems_isnull)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Relation %s attname %s is a scalar type, "
							"cannot have stats of type %s, ignored",
							RelationGetRelationName(rel),
							NameStr(*attname),
							mc_elems_name)));
			mc_elems_isnull = true;
			mc_elem_freqs_isnull = true;
		}

		if (!elem_count_hist_isnull)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Relation %s attname %s is a scalar type, "
							"cannot have stats of type %s, ignored",
							RelationGetRelationName(rel),
							NameStr(*attname),
							elem_count_hist_name)));
			elem_count_hist_isnull = true;
		}
	}

	/* Only range types can have range stats */
	if ((typcache->typtype != TYPTYPE_RANGE) &&
		(typcache->typtype != TYPTYPE_MULTIRANGE))
	{
		/* warn and ignore any stats that can't be fulfilled */
		if (!range_length_hist_isnull)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Relation %s attname %s is not a range type, "
							"cannot have stats of type %s",
							RelationGetRelationName(rel),
							NameStr(*attname),
							range_length_hist_name)));
			range_length_hist_isnull = true;
			range_empty_frac_isnull = true;
		}

		if (!range_bounds_hist_isnull)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Relation %s attname %s is not a range type, "
							"cannot have stats of type %s",
							RelationGetRelationName(rel),
							NameStr(*attname),
							range_bounds_hist_name)));
			range_bounds_hist_isnull = true;
		}
	}

	if (!check_can_update_stats(rel))
	{
		relation_close(rel, NoLock);
		ereport(elevel,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for relation %s",
						RelationGetRelationName(rel))));
		return false;
	}

	/* Populate pg_statistic tuple */
	values[Anum_pg_statistic_starelid - 1] = relation_datum;
	values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(attnum);
	values[Anum_pg_statistic_stainherit - 1] = inherited_datum;
	values[Anum_pg_statistic_stanullfrac - 1] = null_frac_datum;
	values[Anum_pg_statistic_stawidth - 1] = avg_width_datum;
	values[Anum_pg_statistic_stadistinct - 1] = n_distinct_datum;

	fmgr_info(F_ARRAY_IN, &finfo);

	/*
	 * STATISTIC_KIND_MCV
	 *
	 * most_common_vals: ANYARRAY::text most_common_freqs: real[]
	 */
	if (!mc_vals_isnull)
	{
		Datum		stakind = Int16GetDatum(STATISTIC_KIND_MCV);
		Datum		staop = ObjectIdGetDatum(typcache->eq_opr);
		Datum		stacoll = ObjectIdGetDatum(typcoll);
		bool		converted = false;
		Datum		stanumbers = mc_freqs_datum;
		Datum		stavalues = cast_stavalues(&finfo, mc_vals_datum,
											   typcache->type_id, typmod,
											   elevel, &converted);

		if (converted &&
			array_check(stavalues, false, mc_vals_name, elevel) &&
			array_check(stanumbers, true, mc_freqs_name, elevel))
		{
			values[Anum_pg_statistic_stakind1 - 1 + stakindidx] = stakind;
			values[Anum_pg_statistic_staop1 - 1 + stakindidx] = staop;
			values[Anum_pg_statistic_stacoll1 - 1 + stakindidx] = stacoll;
			values[Anum_pg_statistic_stanumbers1 - 1 + stakindidx] = stanumbers;
			values[Anum_pg_statistic_stavalues1 - 1 + stakindidx] = stavalues;

			stakindidx++;
		}
		else
		{
			/* Mark as skipped */
			mc_vals_isnull = true;
			mc_freqs_isnull = true;
		}
	}

	/*
	 * STATISTIC_KIND_HISTOGRAM
	 *
	 * histogram_bounds: ANYARRAY::text
	 */
	if (!histogram_bounds_isnull)
	{
		Datum		stakind = Int16GetDatum(STATISTIC_KIND_HISTOGRAM);
		Datum		staop = ObjectIdGetDatum(typcache->lt_opr);
		Datum		stacoll = ObjectIdGetDatum(typcoll);
		Datum		stavalues;
		bool		converted = false;

		stavalues = cast_stavalues(&finfo, histogram_bounds_datum,
								   typcache->type_id, typmod, elevel,
								   &converted);

		if (converted &&
			array_check(stavalues, false, histogram_bounds_name, elevel))
		{
			values[Anum_pg_statistic_stakind1 - 1 + stakindidx] = stakind;
			values[Anum_pg_statistic_staop1 - 1 + stakindidx] = staop;
			values[Anum_pg_statistic_stacoll1 - 1 + stakindidx] = stacoll;
			nulls[Anum_pg_statistic_stanumbers1 - 1 + stakindidx] = true;
			values[Anum_pg_statistic_stavalues1 - 1 + stakindidx] = stavalues;

			stakindidx++;
		}
		else
			histogram_bounds_isnull = true;
	}

	/*
	 * STATISTIC_KIND_CORRELATION
	 *
	 * correlation: real
	 */
	if (!correlation_isnull)
	{
		Datum		stakind = Int16GetDatum(STATISTIC_KIND_CORRELATION);
		Datum		staop = ObjectIdGetDatum(typcache->lt_opr);
		Datum		stacoll = ObjectIdGetDatum(typcoll);
		Datum		elems[] = {correlation_datum};
		ArrayType  *arry = construct_array_builtin(elems, 1, FLOAT4OID);
		Datum		stanumbers = PointerGetDatum(arry);

		values[Anum_pg_statistic_stakind1 - 1 + stakindidx] = stakind;
		values[Anum_pg_statistic_staop1 - 1 + stakindidx] = staop;
		values[Anum_pg_statistic_stacoll1 - 1 + stakindidx] = stacoll;
		values[Anum_pg_statistic_stanumbers1 - 1 + stakindidx] = stanumbers;
		nulls[Anum_pg_statistic_stavalues1 - 1 + stakindidx] = true;

		stakindidx++;
	}

	/*
	 * STATISTIC_KIND_MCELEM
	 *
	 * most_common_elem_freqs: real[]
	 *
	 * most_common_elems     : ANYARRAY::text
	 */
	if (!mc_elems_isnull)
	{
		Datum		stakind = Int16GetDatum(STATISTIC_KIND_MCELEM);
		Datum		staop = ObjectIdGetDatum(elemtypcache->eq_opr);
		Datum		stacoll = ObjectIdGetDatum(typcoll);
		Datum		stanumbers = mc_elem_freqs_datum;
		bool		converted = false;
		Datum		stavalues;

		stavalues = cast_stavalues(&finfo, mc_elems_datum,
								   elemtypcache->type_id, typmod,
								   elevel, &converted);

		if (converted &&
			array_check(stavalues, false, mc_elems_name, elevel) &&
			array_check(stanumbers, true, mc_elem_freqs_name, elevel))
		{
			values[Anum_pg_statistic_stakind1 - 1 + stakindidx] = stakind;
			values[Anum_pg_statistic_staop1 - 1 + stakindidx] = staop;
			values[Anum_pg_statistic_stacoll1 - 1 + stakindidx] = stacoll;
			values[Anum_pg_statistic_stanumbers1 - 1 + stakindidx] = stanumbers;
			values[Anum_pg_statistic_stavalues1 - 1 + stakindidx] = stavalues;

			stakindidx++;
		}
		else
		{
			/* the mc_elem stat did not write */
			mc_elems_isnull = true;
			mc_elem_freqs_isnull = true;
		}
	}

	/*
	 * STATISTIC_KIND_DECHIST
	 *
	 * elem_count_histogram:	real[]
	 */
	if (!elem_count_hist_isnull)
	{
		Datum		stakind = Int16GetDatum(STATISTIC_KIND_DECHIST);
		Datum		staop = ObjectIdGetDatum(elemtypcache->eq_opr);
		Datum		stacoll = ObjectIdGetDatum(typcoll);
		Datum		stanumbers = elem_count_hist_datum;

		values[Anum_pg_statistic_stakind1 - 1 + stakindidx] = stakind;
		values[Anum_pg_statistic_staop1 - 1 + stakindidx] = staop;
		values[Anum_pg_statistic_stacoll1 - 1 + stakindidx] = stacoll;
		values[Anum_pg_statistic_stanumbers1 - 1 + stakindidx] = stanumbers;
		nulls[Anum_pg_statistic_stavalues1 - 1 + stakindidx] = true;

		stakindidx++;
	}

	/*
	 * STATISTIC_KIND_BOUNDS_HISTOGRAM
	 *
	 * range_bounds_histogram: ANYARRAY::text
	 *
	 * This stakind appears before STATISTIC_KIND_BOUNDS_HISTOGRAM even though
	 * it is numerically greater, and all other stakinds appear in numerical
	 * order. We duplicate this quirk to make before/after tests of
	 * pg_statistic records easier.
	 */
	if (!range_bounds_hist_isnull)
	{
		Datum		stakind = Int16GetDatum(STATISTIC_KIND_BOUNDS_HISTOGRAM);
		Datum		staop = ObjectIdGetDatum(InvalidOid);
		Datum		stacoll = ObjectIdGetDatum(InvalidOid);

		bool		converted = false;
		Datum		stavalues;

		stavalues = cast_stavalues(&finfo, range_bounds_hist_datum,
								   typcache->type_id, typmod,
								   elevel, &converted);

		if (converted &&
			array_check(stavalues, false, "range_bounds_histogram", elevel))
		{
			values[Anum_pg_statistic_stakind1 - 1 + stakindidx] = stakind;
			values[Anum_pg_statistic_staop1 - 1 + stakindidx] = staop;
			values[Anum_pg_statistic_stacoll1 - 1 + stakindidx] = stacoll;
			nulls[Anum_pg_statistic_stanumbers1 - 1 + stakindidx] = true;
			values[Anum_pg_statistic_stavalues1 - 1 + stakindidx] = stavalues;

			stakindidx++;
		}
		else
			range_bounds_hist_isnull = true;
	}

	/*
	 * STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM
	 *
	 * range_empty_frac: real
	 *
	 * range_length_histogram:  double precision[]::text
	 */
	if (!range_length_hist_isnull)
	{
		Datum		stakind = Int16GetDatum(STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM);
		Datum		staop = ObjectIdGetDatum(Float8LessOperator);
		Datum		stacoll = ObjectIdGetDatum(InvalidOid);

		/* The anyarray is always a float8[] for this stakind */
		Datum		elems[] = {range_empty_frac_datum};
		ArrayType  *arry = construct_array_builtin(elems, 1, FLOAT4OID);
		Datum		stanumbers = PointerGetDatum(arry);

		bool		converted = false;
		Datum		stavalues;

		stavalues = cast_stavalues(&finfo, range_length_hist_datum, FLOAT8OID,
								   0, elevel, &converted);

		if (converted &&
			array_check(stavalues, false, range_length_hist_name, elevel))
		{
			values[Anum_pg_statistic_staop1 - 1 + stakindidx] = staop;
			values[Anum_pg_statistic_stakind1 - 1 + stakindidx] = stakind;
			values[Anum_pg_statistic_stacoll1 - 1 + stakindidx] = stacoll;
			values[Anum_pg_statistic_stanumbers1 - 1 + stakindidx] = stanumbers;
			values[Anum_pg_statistic_stavalues1 - 1 + stakindidx] = stavalues;
			stakindidx++;
		}
		else
		{
			range_empty_frac_isnull = true;
			range_length_hist_isnull = true;
		}
	}

	/* fill in all remaining slots */
	while (stakindidx < STATISTIC_NUM_SLOTS)
	{
		values[Anum_pg_statistic_stakind1 - 1 + stakindidx] = Int16GetDatum(0);
		values[Anum_pg_statistic_staop1 - 1 + stakindidx] = ObjectIdGetDatum(InvalidOid);
		values[Anum_pg_statistic_stacoll1 - 1 + stakindidx] = ObjectIdGetDatum(InvalidOid);
		nulls[Anum_pg_statistic_stanumbers1 - 1 + stakindidx] = true;
		nulls[Anum_pg_statistic_stavalues1 - 1 + stakindidx] = true;

		stakindidx++;
	}

	update_pg_statistic(values, nulls);

	relation_close(rel, NoLock);

	return true;
}

/*
 * Import statistics for a given relation attribute.
 *
 * This will insert/replace a row in pg_statistic for the given relation and
 * attribute name.
 *
 * The function takes input parameters that correspond to columns in the view
 * pg_stats.
 *
 * Of those, the columns attname, inherited, null_frac, avg_width, and
 * n_distinct all correspond to NOT NULL columns in pg_statistic. These
 * parameters have no default value and passing NULL to them will result
 * in an error.
 *
 * If there is no attribute with a matching attname in the relation, the
 * function will raise an error. Likewise for setting inherited statistics
 * on a table that is not partitioned.
 *
 * The remaining parameters all belong to a specific stakind. Some stakinds
 * have multiple parameters, and in those cases both parameters must be
 * NOT NULL or both NULL, otherwise an error will be raised.
 *
 * Omitting a parameter or explicitly passing NULL means that that particular
 * stakind is not associated with the attribute.
 *
 * Parameters that are NOT NULL will be inspected for consistency checks,
 * any of which can raise an error.
 *
 * Parameters corresponding to ANYARRAY columns are instead passed in as text
 * values, which is a valid input string for an array of the type or element
 * type of the attribute. Any error generated by the array_in() function will
 * in turn fail the function.
 */
Datum
pg_set_attribute_stats(PG_FUNCTION_ARGS)
{
	Datum	version_datum = (Datum) 0;
	bool	version_isnull = true;
	bool	raise_errors = true;

	attribute_statistics_update(
		PG_GETARG_DATUM(0), PG_ARGISNULL(0),
		PG_GETARG_DATUM(1), PG_ARGISNULL(1),
		PG_GETARG_DATUM(2), PG_ARGISNULL(2),
		version_datum, version_isnull,
		PG_GETARG_DATUM(3), PG_ARGISNULL(3),
		PG_GETARG_DATUM(4), PG_ARGISNULL(4),
		PG_GETARG_DATUM(5), PG_ARGISNULL(5),
		PG_GETARG_DATUM(6), PG_ARGISNULL(6),
		PG_GETARG_DATUM(7), PG_ARGISNULL(7),
		PG_GETARG_DATUM(8), PG_ARGISNULL(8),
		PG_GETARG_DATUM(9), PG_ARGISNULL(9),
		PG_GETARG_DATUM(10), PG_ARGISNULL(10),
		PG_GETARG_DATUM(11), PG_ARGISNULL(11),
		PG_GETARG_DATUM(12), PG_ARGISNULL(12),
		PG_GETARG_DATUM(13), PG_ARGISNULL(13),
		PG_GETARG_DATUM(14), PG_ARGISNULL(14),
		PG_GETARG_DATUM(15), PG_ARGISNULL(15),
		raise_errors);

	PG_RETURN_VOID();
}
