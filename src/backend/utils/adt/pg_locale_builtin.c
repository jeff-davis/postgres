/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities for builtin provider
 *
 * Portions Copyright (c) 2002-2025, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/pg_locale_builtin.c
 *
 *-----------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_database.h"
#include "catalog/pg_collation.h"
#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "common/unicode_version.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

extern pg_locale_t create_pg_locale_builtin(Oid collid,
											MemoryContext context);
extern char *get_collation_actual_version_builtin(const char *collcollate);

static int strncoll_builtin_ci(const char *arg1, ssize_t len1,
							   const char *arg2, ssize_t len2,
							   pg_locale_t locale);
static size_t strnxfrm_builtin_ci(char *dest, size_t destsize,
								  const char *src, ssize_t srclen,
								  pg_locale_t locale);

struct WordBoundaryState
{
	const char *str;
	size_t		len;
	size_t		offset;
	bool		posix;
	bool		init;
	bool		prev_alnum;
};

/*
 * Simple word boundary iterator that draws boundaries each time the result of
 * pg_u_isalnum() changes.
 */
static size_t
initcap_wbnext(void *state)
{
	struct WordBoundaryState *wbstate = (struct WordBoundaryState *) state;

	while (wbstate->offset < wbstate->len &&
		   wbstate->str[wbstate->offset] != '\0')
	{
		pg_wchar	u = utf8_to_unicode((unsigned char *) wbstate->str +
										wbstate->offset);
		bool		curr_alnum = pg_u_isalnum(u, wbstate->posix);

		if (!wbstate->init || curr_alnum != wbstate->prev_alnum)
		{
			size_t		prev_offset = wbstate->offset;

			wbstate->init = true;
			wbstate->offset += unicode_utf8len(u);
			wbstate->prev_alnum = curr_alnum;
			return prev_offset;
		}

		wbstate->offset += unicode_utf8len(u);
	}

	return wbstate->len;
}

static size_t
strlower_builtin(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	return unicode_strlower(dest, destsize, src, srclen,
							locale->info.builtin.casemap_full);
}

static size_t
strtitle_builtin(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	struct WordBoundaryState wbstate = {
		.str = src,
		.len = srclen,
		.offset = 0,
		.posix = !locale->info.builtin.casemap_full,
		.init = false,
		.prev_alnum = false,
	};

	return unicode_strtitle(dest, destsize, src, srclen,
							locale->info.builtin.casemap_full,
							initcap_wbnext, &wbstate);
}

static size_t
strupper_builtin(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	return unicode_strupper(dest, destsize, src, srclen,
							locale->info.builtin.casemap_full);
}

static size_t
strfold_builtin(char *dest, size_t destsize, const char *src, ssize_t srclen,
				pg_locale_t locale)
{
	return unicode_strfold(dest, destsize, src, srclen,
						   locale->info.builtin.casemap_full);
}

static bool
wc_isdigit_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isdigit(wc, !locale->info.builtin.casemap_full);
}

static bool
wc_isalpha_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isalpha(wc);
}

static bool
wc_isalnum_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isalnum(wc, !locale->info.builtin.casemap_full);
}

static bool
wc_isupper_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isupper(wc);
}

static bool
wc_islower_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_islower(wc);
}

static bool
wc_isgraph_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isgraph(wc);
}

static bool
wc_isprint_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isprint(wc);
}

static bool
wc_ispunct_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_ispunct(wc, !locale->info.builtin.casemap_full);
}

static bool
wc_isspace_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isspace(wc);
}

static bool
char_is_cased_builtin(char ch, pg_locale_t locale)
{
	return IS_HIGHBIT_SET(ch) ||
		(ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static pg_wchar
wc_toupper_builtin(pg_wchar wc, pg_locale_t locale)
{
	return unicode_uppercase_simple(wc);
}

static pg_wchar
wc_tolower_builtin(pg_wchar wc, pg_locale_t locale)
{
	return unicode_lowercase_simple(wc);
}

static const struct collate_methods collate_methods_builtin_ci = {
	.strncoll = strncoll_builtin_ci,
	.strnxfrm = strnxfrm_builtin_ci,
	.strnxfrm_prefix = strnxfrm_builtin_ci,
	.strxfrm_is_safe = true,
};

static const struct ctype_methods ctype_methods_builtin = {
	.strlower = strlower_builtin,
	.strtitle = strtitle_builtin,
	.strupper = strupper_builtin,
	.strfold = strfold_builtin,
	.wc_isdigit = wc_isdigit_builtin,
	.wc_isalpha = wc_isalpha_builtin,
	.wc_isalnum = wc_isalnum_builtin,
	.wc_isupper = wc_isupper_builtin,
	.wc_islower = wc_islower_builtin,
	.wc_isgraph = wc_isgraph_builtin,
	.wc_isprint = wc_isprint_builtin,
	.wc_ispunct = wc_ispunct_builtin,
	.wc_isspace = wc_isspace_builtin,
	.char_is_cased = char_is_cased_builtin,
	.wc_tolower = wc_tolower_builtin,
	.wc_toupper = wc_toupper_builtin,
};

pg_locale_t
create_pg_locale_builtin(Oid collid, MemoryContext context)
{
	const char *locstr;
	pg_locale_t result;

	if (collid == DEFAULT_COLLATION_OID)
	{
		HeapTuple	tp;
		Datum		datum;

		tp = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
		datum = SysCacheGetAttrNotNull(DATABASEOID, tp,
									   Anum_pg_database_datlocale);
		locstr = TextDatumGetCString(datum);
		ReleaseSysCache(tp);
	}
	else
	{
		HeapTuple	tp;
		Datum		datum;

		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);
		datum = SysCacheGetAttrNotNull(COLLOID, tp,
									   Anum_pg_collation_colllocale);
		locstr = TextDatumGetCString(datum);
		ReleaseSysCache(tp);
	}

	builtin_validate_locale(GetDatabaseEncoding(), locstr);

	result = MemoryContextAllocZero(context, sizeof(struct pg_locale_struct));

	result->info.builtin.locale = MemoryContextStrdup(context, locstr);
	result->info.builtin.casemap_full = (strcmp(locstr, "PG_UNICODE_FAST") == 0) ||
		(strcmp(locstr, "PG_UNICODE_CI") == 0);
	result->deterministic = (strcmp(locstr, "PG_UNICODE_CI") != 0);
	result->collate_is_c = (strcmp(locstr, "PG_UNICODE_CI") != 0);
	if (!result->collate_is_c)
		result->collate = &collate_methods_builtin_ci;
	result->ctype_is_c = (strcmp(locstr, "C") == 0);
	if (!result->ctype_is_c)
		result->ctype = &ctype_methods_builtin;

	return result;
}

char *
get_collation_actual_version_builtin(const char *collcollate)
{
	/*
	 * Locales C, C.UTF-8, and PG_UNICODE_FAST are based on memcmp and are not
	 * expected to change, but track the version anyway.
	 *
	 * PG_UNICODE_CI has collation behavior dependent on the version of
	 * Unicode, so use that for the collation version.
	 *
	 * Note that the character semantics may change for some locales, but the
	 * collation version only tracks changes to sort order.
	 */
	if (strcmp(collcollate, "C") == 0)
		return "1";
	else if (strcmp(collcollate, "C.UTF-8") == 0)
		return "1";
	else if (strcmp(collcollate, "PG_UNICODE_FAST") == 0)
		return "1";
	else if (strcmp(collcollate, "PG_UNICODE_CI") == 0)
		return PG_UNICODE_VERSION;
	else
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("invalid locale name \"%s\" for builtin provider",
						collcollate)));

	return NULL;				/* keep compiler quiet */
}

/*
 * strncoll_builtin_ci
 *
 * Compare two strings such that the result is equivalent to
 * strcmp(CASEFOLD(arg1), CASEFOLD(arg2)).
 */
int
strncoll_builtin_ci(const char *arg1, ssize_t len1, const char *arg2, ssize_t len2,
					pg_locale_t locale)
{
	char		 buf1[UNICODE_CASEMAP_BUFSZ];
	char		 buf2[UNICODE_CASEMAP_BUFSZ];
	size_t		 nbytes1 = 0; /* bytes stored in buf1 */
	size_t		 nbytes2 = 0; /* bytes stored in buf2 */
	const char	*p1	   = arg1;
	const char	*p2	   = arg2;
	ssize_t		 r1	   = (len1 >= 0) ? len1 : strlen(arg1);
	ssize_t		 r2	   = (len2 >= 0) ? len2 : strlen(arg2);
	const bool	 full  = locale->info.builtin.casemap_full;

	/*
	 * Folding the entire string at once could be wasteful. Fold the arguments
	 * lazily into buf1 and buf2; comparing until we find a difference.
	 *
	 * We need to keep track of buffer contents across iterations, because
	 * folding from the two inputs could result in different numbers of output
	 * bytes (or even different numbers of codepoints). If the comparison is
	 * still inconclusive, we need to keep the remaining bytes around for the
	 * next iteration.
	 */
	while ((r1 > 0 || nbytes1 > 0) && (r2 > 0 || nbytes2 > 0))
	{
		int		ulen1 = 0;
		int		ulen2 = 0;
		size_t	nbytes_both;
		int		result;

		Assert(nbytes1 == 0 || nbytes2 == 0);

		/* if a buffer is empty, fold one codepoint */
		if (nbytes1 == 0)
		{
			ulen1 = pg_utf_mblen((unsigned char *)p1);
			Assert(ulen1 <= r1);
			nbytes1 = unicode_strfold(buf1, UNICODE_CASEMAP_BUFSZ, p1, ulen1,
									  full);
		}

		if (nbytes2 == 0)
		{
			ulen2 = pg_utf_mblen((unsigned char *)p2);
			Assert(ulen2 <= r2);
			nbytes2 = unicode_strfold(buf2, UNICODE_CASEMAP_BUFSZ, p2, ulen2,
									  full);
		}

		Assert(nbytes1 > 0 && nbytes1 <= UNICODE_CASEMAP_BUFSZ);
		Assert(nbytes2 > 0 && nbytes2 <= UNICODE_CASEMAP_BUFSZ);

		/* compare the corresponding bytes available in both buffers */
		nbytes_both = Min(nbytes1, nbytes2);
		result = memcmp(buf1, buf2, nbytes_both);

		if (result != 0)
			return result;

		/* shift any remaining bytes in the buffers to the beginning */
		nbytes1 -= nbytes_both;
		nbytes2 -= nbytes_both;
		memmove(buf1, buf1 + nbytes_both, nbytes1);
		memmove(buf2, buf2 + nbytes_both, nbytes2);

		p1 += ulen1;
		r1 -= ulen1;
		p2 += ulen2;
		r2 -= ulen2;
	}

	if ((r1 == 0 && nbytes1 == 0) && !(r2 == 0 && nbytes2 == 0))
		return -1; /* arg1 exhausted */
	else if (!(r1 == 0 && nbytes1 == 0) && (r2 == 0 && nbytes2 == 0))
		return 1; /* arg2 exhausted */
	else
		return 0; /* both inputs exhausted */
}

/* 'srclen' of -1 means the strings are NUL-terminated */
size_t
strnxfrm_builtin_ci(char *dest, size_t destsize, const char *src, ssize_t srclen,
					pg_locale_t locale)
{
	return unicode_strfold(dest, destsize, src, srclen,
						   locale->info.builtin.casemap_full);
}
