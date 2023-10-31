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
static size_t convert_case(char *dst, size_t dstsize, const char *src, size_t srclen,
						   CaseKind top_casekind, bool real_titlecase, bool full,
						   WordBoundaryNext wbnext, void *wbstate);
static bool check_special_conditions(int conditions, const char *str,
									 size_t len, size_t offset);

pg_wchar
unicode_lowercase_simple(pg_wchar code)
{
	const		pg_case_map *map = find_case_map(code);

	return map ? map->simplemap[CaseLower] : code;
}

pg_wchar
unicode_titlecase_simple(pg_wchar code)
{
	const		pg_case_map *map = find_case_map(code);

	return map ? map->simplemap[CaseTitle] : code;
}

pg_wchar
unicode_uppercase_simple(pg_wchar code)
{
	const		pg_case_map *map = find_case_map(code);

	return map ? map->simplemap[CaseUpper] : code;
}

/*
 * unicode_strlower(), unicode_strtitle(), unicode_strupper()
 *
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
unicode_strlower(char *dst, size_t dstsize, const char *src, size_t srclen,
				 bool full)
{
	return convert_case(dst, dstsize, src, srclen, CaseLower, false, full, NULL, NULL);
}

size_t
unicode_strtitle(char *dst, size_t dstsize, const char *src, size_t srclen,
				 bool real_titlecase, bool full, WordBoundaryNext wbnext, void *wbstate)
{
	return convert_case(dst, dstsize, src, srclen, CaseTitle, real_titlecase, full, wbnext, wbstate);
}

size_t
unicode_strupper(char *dst, size_t dstsize, const char *src, size_t srclen,
				 bool full)
{
	return convert_case(dst, dstsize, src, srclen, CaseUpper, false, full, NULL, NULL);
}

/*
 * Implement Unicode Default Case Conversion algorithm.
 *
 * Titlecasing requires knowledge about word boundaries, which is provided by
 * a callback wbnext. The caller is expected to initialize and free the
 * callback state wbstate. The callback should first return offset 0 for the
 * first boundary, then the offset of each subsequent word boundary, then the
 * total length of the string to indicate the final boundary. A word boundary
 * is either the offset of the start of a word or the offset of the first
 * character after a word completes.
 *
 * The titlecasing algorithm is as follows: for each word boundary, skip
 * conversion until the first Cased character is found, and map it to
 * titlecase. Then map the remaining characters to lowercase until the next
 * word boundary is found. Other case conversions simply map all of the
 * characters to the requested case, if a mapping exists.
 *
 * In most cases, mapping is a simple one-to-one conversion. But some special
 * mappings exist which map one character to multiple code points, or have
 * conditional mappings that may depend on context.
 */
static size_t
convert_case(char *dst, size_t dstsize, const char *src, size_t srclen,
			 CaseKind top_casekind, bool real_titlecase, bool full,
			 WordBoundaryNext wbnext, void *wbstate)
{
	size_t		srcoff = 0;
	size_t		result_len = 0;
	size_t		boundary = 0;
	bool		find_next_cased = true;
	CaseKind	casekind = top_casekind;

	/* need word boundaries for titlecasing */
	if (top_casekind == CaseTitle)
		boundary = wbnext(wbstate);

	while (src[srcoff] != '\0' && (srclen < 0 || srcoff < srclen))
	{
		pg_wchar	u1 = utf8_to_unicode((unsigned char *) src + srcoff);
		int			u1len = unicode_utf8len(u1);
		const		pg_case_map *casemap = NULL;
		const		pg_special_case *special = NULL;

		/*
		 * Titlecasing has two states: searching for the next Cased character;
		 * and searching for the next word boundary.
		 *
		 * While searching for the next Cased character, just copy the bytes
		 * from src without conversion, until we find the next Cased
		 * character, which is mapped to titlecase. While searching for the
		 * next word boundary, map each character to lowercase.
		 */
		if (top_casekind == CaseTitle)
		{
			if (srcoff == boundary)
			{
				/* switch state to finding next Cased character */
				find_next_cased = true;
				boundary = wbnext(wbstate);
			}

			if (find_next_cased && pg_u_prop_cased(u1))
			{
				/* titlecase this character, then switch to lowercasing */
				find_next_cased = false;
				casekind = real_titlecase ? CaseTitle : CaseUpper;
				casemap = find_case_map(u1);
			}
			else if (!find_next_cased)
			{
				/* lowercase this character */
				casekind = CaseLower;
				casemap = find_case_map(u1);
			}
			else
			{
				/* copy from src without conversion */
				casemap = NULL;
			}
		}
		else
			casemap = find_case_map(u1);


		/*
		 * Find special case that matches the conditions, if any.
		 *
		 * Only a single special mapping per codepoint is currently supported.
		 * Unicode allows for multiple special mappings for a single
		 * codepoint, so we may have a reason to support that in the future.
		 */
		if (full && casemap && casemap->special_case)
		{
			int16		conditions = casemap->special_case->conditions;

			Assert(casemap->special_case->codepoint == u1);
			if (check_special_conditions(conditions, src, srclen, srcoff))
				special = casemap->special_case;
		}

		/* perform mapping, update result_len, and write to dst */
		if (special)
		{
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
