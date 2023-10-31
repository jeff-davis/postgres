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
#include "common/unicode_category.h"
#include "mb/pg_wchar.h"

static const pg_case_map *find_case_map(pg_wchar ucs);
static bool check_special_conditions(int conditions, const char *str,
									 size_t len, size_t offset);

/* find entry in simple case map, if any */
static const pg_case_map *
find_case_map(pg_wchar ucs)
{
	int			min = 0;
	int			mid;
	int			max = lengthof(case_map) - 1;

	/* all chars <= 0x80 are stored in array for fast lookup */
	Assert(max >= 0x7f);
	if (ucs < 0x80)
	{
		const		pg_case_map *map = &case_map[ucs];

		Assert(map->codepoint == ucs);
		return map;
	}

	/* otherwise, binary search */
	while (max >= min)
	{
		mid = (min + max) / 2;
		if (ucs > case_map[mid].codepoint)
			min = mid + 1;
		else if (ucs < case_map[mid].codepoint)
			max = mid - 1;
		else
			return &case_map[mid];
	}

	return NULL;
}


/*
 * Returns simple mapping for the given character, or the original character
 * if none.
 */
pg_wchar
unicode_case_simple(pg_wchar code, CaseKind casekind)
{
	const		pg_case_map *map = find_case_map(code);

	return map ? map->simplemap[casekind] : code;
}

/*
 * Convert case of src, and return the result length (not including
 * terminating NUL).
 *
 * String src must be encoded in UTF-8. If srclen < 0, src must be
 * NUL-terminated.
 *
 * Result string is stored in dst, truncating if larger than dstsize. If
 * dstsize is greater than the result length, dst will be NUL-terminated;
 * otherwise not.
 *
 * If dstsize is zero, dst may be NULL. This is useful for calculating the
 * required buffer size before allocating.
 */
size_t
unicode_convert_case(char *dst, size_t dstsize, const char *src,
					 size_t srclen, CaseKind casekind, bool full)
{
	size_t		srcoff = 0;
	size_t		result_len = 0;

	/* not currently supported */
	Assert(casekind != CaseTitle);

	while (src[srcoff] != '\0' && (srclen < 0 || srcoff < srclen))
	{
		pg_wchar	u1 = utf8_to_unicode((unsigned char *) src + srcoff);
		int			u1len = unicode_utf8len(u1);
		const		pg_case_map *casemap = find_case_map(u1);
		const		pg_special_case *special = NULL;

		/*
		 * Find special case that matches the conditions, if available.
		 *
		 * Only a single special mapping per codepoint is currently supported.
		 * Unicode allows for multiple special mappings for a single
		 * codepoint, so we may have a reason to support that in the future.
		 */
		if (full && casemap && casemap->special_case)
		{
			int16		conditions = casemap->special_case->conditions;

			if (check_special_conditions(conditions, src, srclen, srcoff))
				special = casemap->special_case;
		}

		/* perform mapping, update result_len, and write to dst */
		if (special)
		{
			/* special mapping available */

			Assert(special->codepoint == u1);

			for (int i = 0; i < MAX_CASE_EXPANSION; i++)
			{
				pg_wchar	u2 = special->map[casekind][i];
				size_t		u2len = unicode_utf8len(u2);

				if (u2 == '\0')
					break;

				if (result_len + u2len < dstsize)
					unicode_to_utf8(u2, (unsigned char *) dst + result_len);
				result_len += u2len;
			}
		}
		else if (casemap)
		{
			/* simple mapping available */

			pg_wchar	u2 = casemap->simplemap[casekind];
			pg_wchar	u2len = unicode_utf8len(u2);

			if (result_len + u2len < dstsize)
				unicode_to_utf8(u2, (unsigned char *) dst + result_len);
			result_len += u2len;
		}
		else
		{
			/* no mapping; copy bytes from src */

			if (result_len + u1len < dstsize)
				memcpy(dst + result_len, src + srcoff, u1len);
			result_len += u1len;
		}

		srcoff += u1len;
	}

	if (result_len < dstsize)
		dst[result_len] = '\0';

	return result_len;
}

/*
 * Check that the condition matches Final_Sigma, described in Unicode Table
 * 3-17. The character at the given offset must be directly preceded by a
 * Cased character, and must not be directly followed by a Cased character.
 *
 * Case_Ignorable characters are ignored. NB: some characters may be both
 * Cased and Case_Ignorable, in which case they are ignored.
 */
static bool
check_final_sigma(const unsigned char *str, size_t len, size_t offset)
{
	/* the start of the string is not preceded by a Cased character */
	if (offset == 0)
		return false;

	/* iterate backwards, looking for Cased character */
	for (int i = offset - 1; i >= 0; i--)
	{
		if ((str[i] & 0x80) == 0 || (str[i] & 0xC0) == 0xC0)
		{
			pg_wchar	curr = utf8_to_unicode(str + i);

			if (pg_u_prop_case_ignorable(curr))
				continue;
			else if (pg_u_prop_cased(curr))
				break;
			else
				return false;
		}
		else if ((str[i] & 0xC0) == 0x80)
			continue;

		Assert(false);			/* invalid UTF-8 */
	}

	/* end of string is not followed by a Cased character */
	if (offset == len)
		return true;

	/* iterate forwards, looking for Cased character */
	for (int i = offset + 1; i < len && str[i] != '\0'; i++)
	{
		if ((str[i] & 0x80) == 0 || (str[i] & 0xC0) == 0xC0)
		{
			pg_wchar	curr = utf8_to_unicode(str + i);

			if (pg_u_prop_case_ignorable(curr))
				continue;
			else if (pg_u_prop_cased(curr))
				return false;
			else
				break;
		}
		else if ((str[i] & 0xC0) == 0x80)
			continue;

		Assert(false);			/* invalid UTF-8 */
	}

	return true;
}

static bool
check_special_conditions(int conditions, const char *str, size_t len,
						 size_t offset)
{
	if (conditions == 0)
		return true;
	else if (conditions == PG_U_FINAL_SIGMA)
		return check_final_sigma((unsigned char *) str, len, offset);

	/* no other conditions supported */
	Assert(false);
	return false;
}
