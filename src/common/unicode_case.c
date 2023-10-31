/*-------------------------------------------------------------------------
 * unicode_case.c
 *		Conversion to upper or lower case.
 *
 * Portions Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/unicode_case.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/unicode_case.h"
#include "common/unicode_case_table.h"

/* binary search to find entry in simple case map, if any */
static const pg_simple_case_map *
find_case_map(pg_wchar ucs)
{
	int			min = 0;
	int			mid;
	int			max = lengthof(simple_case_map) - 1;

	while (max >= min)
	{
		mid = (min + max) / 2;
		if (ucs > simple_case_map[mid].codepoint)
			min = mid + 1;
		else if (ucs < simple_case_map[mid].codepoint)
			max = mid - 1;
		else
			return &simple_case_map[mid];
	}

	return NULL;
}

/*
 * Returns simple lowercase mapping for the given character, or the original
 * character if none. Sets *special to the special case mapping, if any.
 */
pg_wchar
unicode_lowercase(pg_wchar ucs, const pg_wchar **special)
{
	const		pg_simple_case_map *map = find_case_map(ucs);

	if (special)
	{
		if (map && map->special_case)
			*special = map->special_case->lowercase;
		else
			*special = NULL;
	}
	if (map && map->simple_lowercase)
		return map->simple_lowercase;
	return ucs;
}

/*
 * Returns simple titlecase mapping for the given character, or the original
 * character if none. Sets *special to the special case mapping, if any.
 */
pg_wchar
unicode_titlecase(pg_wchar ucs, const pg_wchar **special)
{
	const		pg_simple_case_map *map = find_case_map(ucs);

	if (special)
	{
		if (map && map->special_case)
			*special = map->special_case->titlecase;
		else
			*special = NULL;
	}
	if (map && map->simple_titlecase)
		return map->simple_titlecase;
	return ucs;
}

/*
 * Returns simple uppercase mapping for the given character, or the original
 * character if none. Sets *special to the special case mapping, if any.
 */
pg_wchar
unicode_uppercase(pg_wchar ucs, const pg_wchar **special)
{
	const		pg_simple_case_map *map = find_case_map(ucs);

	if (special)
	{
		if (map && map->special_case)
			*special = map->special_case->uppercase;
		else
			*special = NULL;
	}
	if (map && map->simple_uppercase)
		return map->simple_uppercase;
	return ucs;
}
