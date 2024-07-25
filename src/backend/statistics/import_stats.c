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
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

/*
 * A role has privileges to set statistics on the relation if any of the
 * following are true:
 *   - the role owns the current database and the relation is not shared
 *   - the role has the MAINTAIN privilege on the relation
 *
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
