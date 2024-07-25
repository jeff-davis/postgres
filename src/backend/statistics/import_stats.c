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
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

static bool relation_statistics_update(FunctionCallInfo fcinfo,
									   int version, int elevel);
static void check_privileges(Relation rel);

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
