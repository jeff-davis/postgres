/*-------------------------------------------------------------------------
 * import_stats.c
 *
 *	  PostgreSQL statistics import
 *
 * Code supporting the direct importation of relation statistics, similar to
 * what is done by the ANALYZE command.
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

static bool relation_statistics_update(FunctionCallInfo fcinfo,
									   int version, int elevel);
static bool attribute_statistics_update(Oid reloid, AttrNumber attnum, int version,
										int elevel, bool inherited, float null_frac,
										int avg_width, float n_distinct,
										Datum mc_vals_datum, bool mc_vals_isnull,
										Datum mc_freqs_datum, bool mc_freqs_isnull, 
										Datum histogram_bounds_datum, bool histogram_bounds_isnull,
										Datum correlation_datum, bool correlation_isnull,
										Datum mc_elems_datum, bool mc_elems_isnull,
										Datum mc_elem_freqs_datum, bool mc_elem_freqs_isnull,
										Datum elem_count_hist_datum, bool elem_count_hist_isnull,
										Datum range_length_hist_datum, bool range_length_hist_isnull,
										Datum range_empty_frac_datum, bool range_empty_frac_isnull,
										Datum range_bounds_hist_datum, bool range_bounds_hist_isnull);
static Node * get_attr_expr(Relation rel, int attnum);
static void get_attr_stat_type(Relation rel, AttrNumber attnum, int elevel,
							   Oid *atttypid, int32 *atttypmod,
							   char *atttyptype, Oid *atttypcoll,
							   Oid *eq_opr, Oid *lt_opr);
static bool get_elem_stat_type(Oid atttypid, char atttyptype, int elevel,
							   Oid *elemtypid, Oid *elem_eq_opr);
static Datum text_to_stavalues(const char *staname, FmgrInfo *array_in, Datum d,
							   Oid typid, int32 typmod, int elevel, bool *ok);
static void use_stats_slot(Datum *values, bool *nulls, int slotidx,
						   int16 stakind, Oid staop, Oid stacoll,
						   Datum stanumbers, bool stanumbers_isnull,
						   Datum stavalues, bool stavalues_isnull);
static void update_pg_statistic(Datum values[], bool nulls[]);
static bool delete_pg_statistic(Oid reloid, AttrNumber attnum, bool stainherit);
static void check_privileges(Relation rel);
static void check_arg_array(const char *staname, Datum arr, bool *isnull,
							int elevel);
static void check_arg_pair(const char *arg1name, bool *arg1null,
						   const char *arg2name, bool *arg2null,
						   int elevel);

/*
 * Internal function for modifying statistics for a relation.
 */
static bool
relation_statistics_update(FunctionCallInfo fcinfo, int version, int elevel)
{
	Oid				reloid;
	Relation		relation;
	Relation		crel;
	HeapTuple		ctup;
	Form_pg_class	pgcform;
	int				replaces[3]	= {0};
	Datum			values[3]	= {0};
	bool			nulls[3]	= {false, false, false};
	int				ncols		= 0;
	TupleDesc		tupdesc;
	HeapTuple		newtup;


	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be NULL")));
	else
		reloid = PG_GETARG_OID(0);

	/*
	 * Open relation with ShareUpdateExclusiveLock, consistent with
	 * ANALYZE. Only needed for permission check and then we close it (but
	 * retain the lock).
	 */
	relation = table_open(reloid, ShareUpdateExclusiveLock);

	check_privileges(relation);

	table_close(relation, NoLock);

	/*
	 * Take RowExclusiveLock on pg_class, consistent with
	 * vac_update_relstats().
	 */
	crel = table_open(RelationRelationId, RowExclusiveLock);

	tupdesc = RelationGetDescr(crel);
	ctup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(reloid));
	if (!HeapTupleIsValid(ctup))
	{
		ereport(elevel,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("pg_class entry for relid %u not found", reloid)));
		table_close(crel, RowExclusiveLock);
		return false;
	}

	pgcform = (Form_pg_class) GETSTRUCT(ctup);

	/* relpages */
	if (!PG_ARGISNULL(1))
	{
		int32 relpages = PG_GETARG_INT32(1);

		if (relpages < 0)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("relpages cannot be < 0")));
			table_close(crel, RowExclusiveLock);
			return false;
		}

		if (relpages != pgcform->relpages)
		{
			replaces[ncols] = Anum_pg_class_relpages;
			values[ncols] = Int32GetDatum(relpages);
			ncols++;
		}
	}

	if (!PG_ARGISNULL(2))
	{
		float reltuples = PG_GETARG_FLOAT4(2);

		if (reltuples < -1.0)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("reltuples cannot be < -1.0")));
			table_close(crel, RowExclusiveLock);
			return false;
		}

		if (reltuples != pgcform->reltuples)
		{
			replaces[ncols] = Anum_pg_class_reltuples;
			values[ncols] = Float4GetDatum(reltuples);
			ncols++;
		}
	}

	if (!PG_ARGISNULL(3))
	{
		int32 relallvisible = PG_GETARG_INT32(3);

		if (relallvisible < 0)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("relallvisible cannot be < 0")));
			table_close(crel, RowExclusiveLock);
			return false;
		}

		if (relallvisible != pgcform->relallvisible)
		{
			replaces[ncols] = Anum_pg_class_relallvisible;
			values[ncols] = Int32GetDatum(relallvisible);
			ncols++;
		}
	}

	/* only update pg_class if there is a meaningful change */
	if (ncols == 0)
	{
		table_close(crel, RowExclusiveLock);
		return false;
	}

	newtup = heap_modify_tuple_by_cols(ctup, tupdesc, ncols, replaces, values,
									   nulls);

	CatalogTupleUpdate(crel, &newtup->t_self, newtup);
	heap_freetuple(newtup);

	/* release the lock, consistent with vac_update_relstats() */
	table_close(crel, RowExclusiveLock);

	return true;
}

/*
 * Insert or Update Attribute Statistics
 *
 * Major errors, such as the table not existing, the attribute not existing,
 * or a permissions failure are always reported at ERROR. Other errors, such
 * as a conversion failure, are reported at 'elevel', and a partial update
 * will result.
 *
 * See pg_statistic.h for an explanation of how each statistic kind is
 * stored. Custom statistics kinds are not supported.
 *
 * Depending on the statistics kind, we need to derive information from the
 * attribute for which we're storing the stats. For instance, the MCVs are
 * stored as an anyarray, and the representation of the array needs to store
 * the correct element type, which must be derived from the attribute.
 */
static bool
attribute_statistics_update(Oid reloid, AttrNumber attnum, int version,
							int elevel, bool inherited, float null_frac,
							int avg_width, float n_distinct,
							Datum mc_vals_datum, bool mc_vals_isnull,
							Datum mc_freqs_datum, bool mc_freqs_isnull, 
							Datum histogram_bounds_datum, bool histogram_bounds_isnull,
							Datum correlation_datum, bool correlation_isnull,
							Datum mc_elems_datum, bool mc_elems_isnull,
							Datum mc_elem_freqs_datum, bool mc_elem_freqs_isnull,
							Datum elem_count_hist_datum, bool elem_count_hist_isnull,
							Datum range_length_hist_datum, bool range_length_hist_isnull,
							Datum range_empty_frac_datum, bool range_empty_frac_isnull,
							Datum range_bounds_hist_datum, bool range_bounds_hist_isnull)
{
	Relation	rel;

	Oid			atttypid;
	int32		atttypmod;
	char		atttyptype;
	Oid			atttypcoll;
	Oid			eq_opr;
	Oid			lt_opr;

	Oid			elemtypid;
	Oid			elem_eq_opr;

	FmgrInfo	array_in_fn;

	Datum		values[Natts_pg_statistic];
	bool		nulls[Natts_pg_statistic];

	int			slotidx = 0; /* slot in pg_statistic (1..5), minus one */
	char	   *attname = get_attname(reloid, attnum, false);

	/*
	 * Initialize nulls array to be false for all non-NULL attributes, and
	 * true for all nullable attributes.
	 */
	for (int i = 0; i < Natts_pg_statistic; i++)
	{
		values[i] = (Datum) 0;
		if (i < Anum_pg_statistic_stanumbers1 - 1)
			nulls[i] = false;
		else
			nulls[i] = true;
	}

	check_arg_array("most_common_freqs", mc_freqs_datum,
					&mc_freqs_isnull, elevel);
	check_arg_array("most_common_elem_freqs", mc_elem_freqs_datum,
					&mc_elem_freqs_isnull, elevel);
	check_arg_array("elem_count_histogram", elem_count_hist_datum,
					&elem_count_hist_isnull, elevel);

	/* STATISTIC_KIND_MCV */
	check_arg_pair("most_common_vals", &mc_vals_isnull,
				   "most_common_freqs", &mc_freqs_isnull,
				   elevel);

	/* STATISTIC_KIND_MCELEM */
	check_arg_pair("most_common_elems", &mc_elems_isnull,
				   "most_common_freqs", &mc_elem_freqs_isnull,
				   elevel);

	/* STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM */
	check_arg_pair("range_length_histogram", &range_length_hist_isnull,
				   "range_empty_frac", &range_empty_frac_isnull,
				   elevel);

	rel = relation_open(reloid, ShareUpdateExclusiveLock);

	check_privileges(rel);

	/* derive information from attribute */
	get_attr_stat_type(rel, attnum, elevel,
					   &atttypid, &atttypmod,
					   &atttyptype, &atttypcoll,
					   &eq_opr, &lt_opr);

	/* if needed, derive element type */
	if (!mc_elems_isnull || !elem_count_hist_isnull)
	{
		if (!get_elem_stat_type(atttypid, atttyptype, elevel,
								&elemtypid, &elem_eq_opr))
		{
			ereport(elevel,
					(errmsg("unable to determine element type of attribute \"%s\"", attname),
					 errdetail("Cannot set STATISTIC_KIND_MCELEM or STATISTIC_KIND_DECHIST.")));
			elemtypid = InvalidOid;
			elem_eq_opr = InvalidOid;
			mc_elems_isnull = true;
			elem_count_hist_isnull = true;
		}
	}

	/* histogram and correlation require less-than operator */
	if ((!histogram_bounds_isnull || !correlation_isnull) &&
		!OidIsValid(lt_opr))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine less-than operator for attribute \"%s\"", attname),
				 errdetail("Cannot set STATISTIC_KIND_HISTOGRAM or STATISTIC_KIND_CORRELATION.")));
		histogram_bounds_isnull = true;
		correlation_isnull = true;
	}

	/* only range types can have range stats */
	if ((!range_length_hist_isnull || !range_bounds_hist_isnull) &&
		!(atttyptype == TYPTYPE_RANGE || atttyptype == TYPTYPE_MULTIRANGE))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("attribute \"%s\" is not a range type", attname),
				 errdetail("Cannot set STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM or STATISTIC_KIND_BOUNDS_HISTOGRAM.")));

		range_length_hist_isnull = true;
		range_empty_frac_isnull = true;
	}

	fmgr_info(F_ARRAY_IN, &array_in_fn);

	/* Populate pg_statistic tuple */
	values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(reloid);
	values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(attnum);
	values[Anum_pg_statistic_stainherit - 1] = BoolGetDatum(inherited);
	values[Anum_pg_statistic_stanullfrac - 1] = Float4GetDatum(null_frac);
	values[Anum_pg_statistic_stawidth - 1] = Int32GetDatum(avg_width);
	values[Anum_pg_statistic_stadistinct - 1] = Float4GetDatum(n_distinct);

	/*
	 * STATISTIC_KIND_MCV
	 * 
	 * Convert most_common_vals from text to anyarray, where the element type
	 * is the attribute type, and store in stavalues. Store most_common_freqs
	 * in stanumbers.
	 */
	if (!mc_vals_isnull)
	{
		bool		converted;
		Datum		stanumbers = mc_freqs_datum;
		Datum		stavalues = text_to_stavalues("most_common_vals",
												  &array_in_fn, mc_vals_datum,
												  atttypid, atttypmod,
												  elevel, &converted);

		if (converted)
		{
			use_stats_slot(values, nulls, slotidx++,
						   STATISTIC_KIND_MCV,
						   eq_opr, atttypcoll,
						   stanumbers, false, stavalues, false);
		}
		else
		{
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
		Datum		stavalues;
		bool		converted = false;

		stavalues = text_to_stavalues("histogram_bounds",
									  &array_in_fn, histogram_bounds_datum,
									  atttypid, atttypmod, elevel,
									  &converted);

		if (converted)
		{
			use_stats_slot(values, nulls, slotidx++,
						   STATISTIC_KIND_HISTOGRAM,
						   lt_opr, atttypcoll,
						   0, true, stavalues, false);
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
		Datum		elems[] = {correlation_datum};
		ArrayType  *arry = construct_array_builtin(elems, 1, FLOAT4OID);
		Datum		stanumbers = PointerGetDatum(arry);

		use_stats_slot(values, nulls, slotidx++,
					   STATISTIC_KIND_CORRELATION,
					   lt_opr, atttypcoll,
					   stanumbers, false, 0, true);
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
		Datum		stanumbers = mc_elem_freqs_datum;
		bool		converted = false;
		Datum		stavalues;

		stavalues = text_to_stavalues("most_common_elems",
									  &array_in_fn, mc_elems_datum,
									  elemtypid, atttypmod,
									  elevel, &converted);

		if (converted)
		{
			use_stats_slot(values, nulls, slotidx++,
						   STATISTIC_KIND_MCELEM,
						   elem_eq_opr, atttypcoll,
						   stanumbers, false, stavalues, false);
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
		Datum		stanumbers = elem_count_hist_datum;

		use_stats_slot(values, nulls, slotidx++,
					   STATISTIC_KIND_DECHIST,
					   elem_eq_opr, atttypcoll,
					   stanumbers, false, 0, true);
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
		bool		converted = false;
		Datum		stavalues;

		stavalues = text_to_stavalues("range_bounds_histogram",
									  &array_in_fn, range_bounds_hist_datum,
									  atttypid, atttypmod,
									  elevel, &converted);

		if (converted)
		{
			use_stats_slot(values, nulls, slotidx++,
						   STATISTIC_KIND_BOUNDS_HISTOGRAM,
						   InvalidOid, InvalidOid,
						   0, true, stavalues, false);
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
		/* The anyarray is always a float8[] for this stakind */
		Datum		elems[] = {range_empty_frac_datum};
		ArrayType  *arry = construct_array_builtin(elems, 1, FLOAT4OID);
		Datum		stanumbers = PointerGetDatum(arry);

		bool		converted = false;
		Datum		stavalues;

		stavalues = text_to_stavalues("range_length_histogram",
									  &array_in_fn, range_length_hist_datum,
									  FLOAT8OID, 0, elevel, &converted);

		if (converted)
		{
			use_stats_slot(values, nulls, slotidx++,
						   STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM,
						   Float8LessOperator, InvalidOid,
						   stanumbers, false, stavalues, false);
		}
		else
		{
			range_empty_frac_isnull = true;
			range_length_hist_isnull = true;
		}
	}

	update_pg_statistic(values, nulls);

	relation_close(rel, NoLock);

	return true;
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
 * Derive type information from the attribute.
 */
static void
get_attr_stat_type(Relation rel, AttrNumber attnum, int elevel,
				   Oid *atttypid, int32 *atttypmod,
				   char *atttyptype, Oid *atttypcoll,
				   Oid *eq_opr, Oid *lt_opr)
{
	Oid			relid = RelationGetRelid(rel);
	Form_pg_attribute attr;
	HeapTuple	atup;
	Node	   *expr;
	TypeCacheEntry *typcache;

	atup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid),
						   Int16GetDatum(attnum));

	/* Attribute not found */
	if (!HeapTupleIsValid(atup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("attribute %d of relation with OID %u does not exist",
						attnum, relid)));

	attr = (Form_pg_attribute) GETSTRUCT(atup);

	if (attr->attisdropped)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("attribute %d of relation with OID %u does not exist",
						attnum, RelationGetRelid(rel))));

	expr = get_attr_expr(rel, attr->attnum);

	if (expr == NULL)
	{
		*atttypid = attr->atttypid;
		*atttypmod = attr->atttypmod;
		*atttypcoll = attr->attcollation;
	}
	else
	{
		*atttypid = exprType(expr);
		*atttypmod = exprTypmod(expr);

		/* TODO: better explanation */
		/*
		 * If a collation has been specified for the index column, use that in
		 * preference to anything else; but if not, fall back to whatever we
		 * can get from the expression.
		 */
		if (OidIsValid(attr->attcollation))
			*atttypcoll = attr->attcollation;
		else
			*atttypcoll = exprCollation(expr);
	}
	ReleaseSysCache(atup);

	/* TODO: better explanation */
	/* if it's a multirange, step down to the range type */
	if (type_is_multirange(*atttypid))
		*atttypid = get_multirange_range(*atttypid);

	typcache = lookup_type_cache(*atttypid, TYPECACHE_LT_OPR | TYPECACHE_EQ_OPR);
	*atttyptype = typcache->typtype;
	*eq_opr = typcache->eq_opr;
	*lt_opr = typcache->lt_opr;

	/* TODO: explain special case for tsvector */ 
	if (*atttypid == TSVECTOROID)
		*atttypcoll = DEFAULT_COLLATION_OID;
}

/*
 * Derive element type information from the attribute type.
 */
static bool
get_elem_stat_type(Oid atttypid, char atttyptype, int elevel,
				   Oid *elemtypid, Oid *elem_eq_opr)
{
	TypeCacheEntry *elemtypcache;

	/* TODO: explain special case for tsvector */
	if (atttypid == TSVECTOROID)
		*elemtypid = TEXTOID;
	else if (atttyptype == TYPTYPE_RANGE)
		*elemtypid = get_range_subtype(atttypid);
	else
		*elemtypid = get_base_element_type(atttypid);

	if (!OidIsValid(*elemtypid))
		return false;

	elemtypcache = lookup_type_cache(*elemtypid, TYPECACHE_EQ_OPR);
	if (!OidIsValid(elemtypcache->eq_opr))
		return false;

	*elem_eq_opr = elemtypcache->eq_opr;

	return true;
}

/*
 * Cast a text datum into an array with element type elemtypid.
 *
 * If an error is encountered, capture it and re-throw at elevel, and set ok
 * to false. If the resulting array contains NULLs, raise an error at elevel
 * and set ok to false. Otherwise, set ok to true.
 */
static Datum
text_to_stavalues(const char *staname, FmgrInfo *array_in, Datum d, Oid typid,
				  int32 typmod, int elevel, bool *ok)
{
	LOCAL_FCINFO(fcinfo, 8);
	char	   *s;
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	escontext.details_wanted = true;

	s = TextDatumGetCString(d);

	InitFunctionCallInfoData(*fcinfo, array_in, 3, InvalidOid,
							 (Node *) &escontext, NULL);

	fcinfo->args[0].value = CStringGetDatum(s);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typid);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	pfree(s);

	if (SOFT_ERROR_OCCURRED(&escontext))
	{
		if (elevel != ERROR)
			escontext.error_data->elevel = elevel;
		ThrowErrorData(escontext.error_data);
		*ok = false;
		return (Datum)0;
	}

	if (array_contains_nulls(DatumGetArrayTypeP(result)))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" array cannot contain NULL values", staname)));
		*ok = false;
		return (Datum)0;
	}

	*ok = true;

	return result;
}

static void
use_stats_slot(Datum *values, bool *nulls, int slotidx,
			   int16 stakind, Oid staop, Oid stacoll,
			   Datum stanumbers, bool stanumbers_isnull,
			   Datum stavalues, bool stavalues_isnull)
{
	if (slotidx >= STATISTIC_NUM_SLOTS)
		ereport(ERROR,
				(errmsg("maximum number of statistics slots exceeded: %d", slotidx + 1)));

	/* slot should not be taken */
	Assert(values[Anum_pg_statistic_stakind1 - 1 + slotidx] == (Datum) 0);
	Assert(values[Anum_pg_statistic_staop1 - 1 + slotidx] == (Datum) 0);
	Assert(values[Anum_pg_statistic_stacoll1 - 1 + slotidx] == (Datum) 0);
	Assert(values[Anum_pg_statistic_stanumbers1 - 1 + slotidx] == (Datum) 0);
	Assert(values[Anum_pg_statistic_stavalues1 - 1 + slotidx] == (Datum) 0);

	/* nulls should be false for non-NULL attributes, true for nullable */
	Assert(!nulls[Anum_pg_statistic_stakind1 - 1 + slotidx]);
	Assert(!nulls[Anum_pg_statistic_staop1 - 1 + slotidx]);
	Assert(!nulls[Anum_pg_statistic_stacoll1 - 1 + slotidx]);
	Assert(nulls[Anum_pg_statistic_stanumbers1 - 1 + slotidx]);
	Assert(nulls[Anum_pg_statistic_stavalues1 - 1 + slotidx]);

	values[Anum_pg_statistic_stakind1 - 1 + slotidx] = stakind;
	values[Anum_pg_statistic_staop1 - 1 + slotidx] = staop;
	values[Anum_pg_statistic_stacoll1 - 1 + slotidx] = stacoll;

	if (!stanumbers_isnull)
	{
		values[Anum_pg_statistic_stanumbers1 - 1 + slotidx] = stanumbers;
		nulls[Anum_pg_statistic_stanumbers1 - 1 + slotidx] = false;
	}
	if (!stavalues_isnull)
	{
		values[Anum_pg_statistic_stavalues1 - 1 + slotidx] = stavalues;
		nulls[Anum_pg_statistic_stavalues1 - 1 + slotidx] = false;
	}
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
	table_close(sd, RowExclusiveLock);
}

/*
 * Delete pg_statistic record.
 */
static bool
delete_pg_statistic(Oid reloid, AttrNumber attnum, bool stainherit)
{
	Relation	sd = table_open(StatisticRelationId, RowExclusiveLock);
	HeapTuple	oldtup;

	/* Is there already a pg_statistic tuple for this attribute? */
	oldtup = SearchSysCache3(STATRELATTINH,
							 ObjectIdGetDatum(reloid),
							 Int16GetDatum(attnum),
							 BoolGetDatum(stainherit));

	if (HeapTupleIsValid(oldtup))
	{
		CatalogTupleDelete(sd, &oldtup->t_self);
		ReleaseSysCache(oldtup);
		table_close(sd, RowExclusiveLock);
		return true;
	}

	table_close(sd, RowExclusiveLock);
	return false;
}

/*
 * A role has privileges to set statistics on the relation if any of the
 * following are true:
 *   - the role owns the current database and the relation is not shared
 *   - the role has the MAINTAIN privilege on the relation
 */
static void
check_privileges(Relation rel)
{
	AclResult               aclresult;

	if (object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) &&
		!rel->rd_rel->relisshared)
		return;

	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  ACL_MAINTAIN);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   NameStr(rel->rd_rel->relname));
}

/*
 * Check that array argument is one dimensional with no NULLs.
 */
static void
check_arg_array(const char *staname, Datum datum, bool *isnull, int elevel)
{
	ArrayType  *arr;

	if (*isnull)
		return;

	arr = DatumGetArrayTypeP(datum);

	if (ARR_NDIM(arr) != 1)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" cannot be a multidimensional array", staname)));
		*isnull = true;
	}

	if (array_contains_nulls(arr))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" array cannot contain NULL values", staname)));
		*isnull = true;
	}
}

/*
 * Enforce parameter pairs that must be specified together for a particular
 * stakind, such as most_common_vals and most_common_freqs for
 * STATISTIC_KIND_MCV. If one is NULL and the other is not, emit at elevel,
 * and ignore the stakind by setting both to NULL.
 */
static void
check_arg_pair(const char *arg1name, bool *arg1null,
			   const char *arg2name, bool *arg2null,
			   int elevel)
{
	if (*arg1null && !*arg2null)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" must be specified when \"%s\" is specified",
						arg1name, arg2name)));
		*arg2null = true;
	}
	if (!*arg1null && *arg2null)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" must be specified when \"%s\" is specified",
						arg2name, arg1name)));
		*arg1null = true;
	}
}

/*
 * Set statistics for a given pg_class entry.
 */
Datum
pg_set_relation_stats(PG_FUNCTION_ARGS)
{
	int			version = PG_VERSION_NUM;
	int			elevel = ERROR;

	PG_RETURN_BOOL(relation_statistics_update(fcinfo, version, elevel));
}

/*
 * Clear statistics for a given pg_class entry; that is, set back to initial
 * stats for a newly-created table.
 */
Datum
pg_clear_relation_stats(PG_FUNCTION_ARGS)
{
	int32	relpages	  = 0;
	float	reltuples	  = -1.0;
	int32	relallvisible = 0;
	int		version		  = PG_VERSION_NUM;
	int		elevel		  = ERROR;
	LOCAL_FCINFO(newfcinfo, 4);

	InitFunctionCallInfoData(*newfcinfo, NULL, 4, InvalidOid, NULL, NULL);

	newfcinfo->args[0].value = PG_GETARG_OID(0);
	newfcinfo->args[0].isnull = PG_ARGISNULL(0);
	newfcinfo->args[1].value = Int32GetDatum(relpages);
	newfcinfo->args[1].isnull = false;
	newfcinfo->args[2].value = Float4GetDatum(reltuples);
	newfcinfo->args[2].isnull = false;
	newfcinfo->args[3].value = Int32GetDatum(relallvisible);
	newfcinfo->args[3].isnull = false;

	PG_RETURN_BOOL(relation_statistics_update(newfcinfo, version, elevel));
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
	Oid			reloid;
	Name		attname;
	AttrNumber	attnum;
	int			version	= PG_VERSION_NUM;
	int			elevel	= ERROR;
	bool		inherited;
	float		null_frac;
	int			avg_width;
	float		n_distinct;
	bool		result;

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be NULL")));
		return false;
	}
	reloid = PG_GETARG_OID(0);

	if (PG_ARGISNULL(1))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("attname cannot be NULL")));
		return false;
	}
	attname = PG_GETARG_NAME(1);
	attnum = get_attnum(reloid, NameStr(*attname));

	if (PG_ARGISNULL(2))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("inherited cannot be NULL")));
		return false;
	}
	inherited = PG_GETARG_BOOL(2);

	if (PG_ARGISNULL(3))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("null_frac cannot be NULL")));
		return false;
	}
	null_frac = PG_GETARG_FLOAT4(3);

	if (PG_ARGISNULL(4))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("avg_width cannot be NULL")));
		return false;
	}
	avg_width = PG_GETARG_INT32(4);

	if (PG_ARGISNULL(5))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("n_distinct cannot be NULL")));
		return false;
	}
	n_distinct = PG_GETARG_FLOAT4(5);

	result = attribute_statistics_update(
		reloid, attnum, version, elevel, inherited,
		null_frac, avg_width, n_distinct,
		PG_GETARG_DATUM(6), PG_ARGISNULL(6),
		PG_GETARG_DATUM(7), PG_ARGISNULL(7),
		PG_GETARG_DATUM(8), PG_ARGISNULL(8),
		PG_GETARG_DATUM(9), PG_ARGISNULL(9),
		PG_GETARG_DATUM(10), PG_ARGISNULL(10),
		PG_GETARG_DATUM(11), PG_ARGISNULL(11),
		PG_GETARG_DATUM(12), PG_ARGISNULL(12),
		PG_GETARG_DATUM(13), PG_ARGISNULL(13),
		PG_GETARG_DATUM(14), PG_ARGISNULL(14),
		PG_GETARG_DATUM(15), PG_ARGISNULL(15));

	PG_RETURN_BOOL(result);
}

/*
 * Delete statistics for the given attribute.
 */
Datum
pg_clear_attribute_stats(PG_FUNCTION_ARGS)
{
	Oid			reloid;
	Relation	relation;
	Name		attname;
	AttrNumber	attnum;
	bool		inherited;

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be NULL")));
		return false;
	}
	reloid = PG_GETARG_OID(0);

	relation = table_open(reloid, ShareUpdateExclusiveLock);

	check_privileges(relation);

	table_close(relation, NoLock);

	if (PG_ARGISNULL(1))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("attname cannot be NULL")));
		return false;
	}
	attname = PG_GETARG_NAME(1);
	attnum = get_attnum(reloid, NameStr(*attname));

	if (PG_ARGISNULL(2))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("inherited cannot be NULL")));
		return false;
	}
	inherited = PG_GETARG_BOOL(2);

	PG_RETURN_BOOL(delete_pg_statistic(reloid, attnum, inherited));
}
