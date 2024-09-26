/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities for libc
 *
 * Portions Copyright (c) 2002-2024, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/pg_locale_libc.c
 *
 *-----------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <wctype.h>

#include "access/htup_details.h"
#include "catalog/pg_database.h"
#include "catalog/pg_collation.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

/*
 * This should be large enough that most strings will fit, but small enough
 * that we feel comfortable putting it on the stack
 */
#define		TEXTBUFLEN			1024

extern pg_locale_t dat_create_locale_libc(HeapTuple dattuple);
extern pg_locale_t coll_create_locale_libc(HeapTuple colltuple,
										   MemoryContext context);

static int	strncoll_libc(const char *arg1, ssize_t len1,
						  const char *arg2, ssize_t len2,
						  pg_locale_t locale);
static size_t strnxfrm_libc(char *dest, size_t destsize,
							const char *src, ssize_t srclen,
							pg_locale_t locale);

static locale_t make_libc_collator(const char *collate,
								   const char *ctype);
static void report_newlocale_failure(const char *localename);

#ifdef WIN32
static int	strncoll_libc_win32_utf8(const char *arg1, ssize_t len1,
									 const char *arg2, ssize_t len2,
									 pg_locale_t locale);
#endif

static size_t strlower_libc(char *dest, size_t destsize,
							const char *src, ssize_t srclen,
							pg_locale_t locale);
static size_t strtitle_libc(char *dest, size_t destsize,
							const char *src, ssize_t srclen,
							pg_locale_t locale);
static size_t strupper_libc(char *dest, size_t destsize,
							const char *src, ssize_t srclen,
							pg_locale_t locale);

static struct collate_methods collate_methods_libc = {
	.strncoll = strncoll_libc,
	.strnxfrm = strnxfrm_libc,
	.strnxfrm_prefix = NULL,

	/*
	 * Unfortunately, it seems that strxfrm() for non-C collations is broken
	 * on many common platforms; testing of multiple versions of glibc reveals
	 * that, for many locales, strcoll() and strxfrm() do not return
	 * consistent results. While no other libc other than Cygwin has so far
	 * been shown to have a problem, we take the conservative course of action
	 * for right now and disable this categorically.  (Users who are certain
	 * this isn't a problem on their system can define TRUST_STRXFRM.)
	 */
#ifdef TRUST_STRXFRM
	.strxfrm_is_safe = true,
#else
	.strxfrm_is_safe = false,
#endif
};

static struct casemap_methods casemap_methods_libc = {
	.strlower = strlower_libc,
	.strtitle = strtitle_libc,
	.strupper = strupper_libc,
};

static size_t
strlower_libc(char *dest, size_t destsize, const char *src, ssize_t srclen,
			  pg_locale_t locale)
{
	locale_t	loc = locale->info.lt;
	size_t		result_size;

	if (pg_database_encoding_max_length() > 1)
	{
		wchar_t    *workspace;
		size_t		curr_char;

		/* Overflow paranoia */
		if ((srclen + 1) > (INT_MAX / sizeof(wchar_t)))
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		/* Output workspace cannot have more codes than input bytes */
		workspace = (wchar_t *) palloc((srclen + 1) * sizeof(wchar_t));

		char2wchar(workspace, srclen + 1, src, srclen, locale);

		for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
			workspace[curr_char] = towlower_l(workspace[curr_char], loc);

		/*
		 * Make result large enough; case change might change number of bytes
		 */
		result_size = curr_char * pg_database_encoding_max_length();
		if (result_size + 1 > destsize)
			return result_size;

		wchar2char(dest, workspace, result_size + 1, locale);
		pfree(workspace);
	}
	else
	{
		char	   *p;

		result_size = srclen;
		if (result_size + 1 > destsize)
			return result_size;

		strlcpy(dest, src, result_size + 1);

		/*
		 * Note: we assume that tolower_l() will not be so broken as to need
		 * an isupper_l() guard test.  When using the default collation, we
		 * apply the traditional Postgres behavior that forces ASCII-style
		 * treatment of I/i, but in non-default collations you get exactly
		 * what the collation says.
		 */
		for (p = dest; *p; p++)
			*p = tolower_l((unsigned char) *p, loc);
	}

	result_size = strlen(dest);
	return result_size;
}

static size_t
strtitle_libc(char *dest, size_t destsize, const char *src, ssize_t srclen,
			  pg_locale_t locale)
{
	locale_t	loc = locale->info.lt;
	int			wasalnum = false;
	size_t		result_size;

	if (pg_database_encoding_max_length() > 1)
	{
		wchar_t    *workspace;
		size_t		curr_char;

		/* Overflow paranoia */
		if ((srclen + 1) > (INT_MAX / sizeof(wchar_t)))
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		/* Output workspace cannot have more codes than input bytes */
		workspace = (wchar_t *) palloc((srclen + 1) * sizeof(wchar_t));

		char2wchar(workspace, srclen + 1, src, srclen, locale);

		for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
		{
			if (wasalnum)
				workspace[curr_char] = towlower_l(workspace[curr_char], loc);
			else
				workspace[curr_char] = towupper_l(workspace[curr_char], loc);
			wasalnum = iswalnum_l(workspace[curr_char], loc);
		}

		/*
		 * Make result large enough; case change might change number of bytes
		 */
		result_size = curr_char * pg_database_encoding_max_length();

		if (result_size + 1 > destsize)
			return result_size;

		wchar2char(dest, workspace, result_size + 1, locale);
		pfree(workspace);
	}
	else
	{
		char	   *p;

		if (srclen + 1 > destsize)
			return srclen;

		strlcpy(dest, src, srclen + 1);

		/*
		 * Note: we assume that toupper_l()/tolower_l() will not be so broken
		 * as to need guard tests.  When using the default collation, we apply
		 * the traditional Postgres behavior that forces ASCII-style treatment
		 * of I/i, but in non-default collations you get exactly what the
		 * collation says.
		 */
		for (p = dest; *p; p++)
		{
			if (wasalnum)
				*p = tolower_l((unsigned char) *p, loc);
			else
				*p = toupper_l((unsigned char) *p, loc);
			wasalnum = isalnum_l((unsigned char) *p, loc);
		}
	}

	result_size = strlen(dest);
	return result_size;
}

static size_t
strupper_libc(char *dest, size_t destsize, const char *src, ssize_t srclen,
			  pg_locale_t locale)
{
	locale_t	loc = locale->info.lt;
	size_t		result_size;

	if (pg_database_encoding_max_length() > 1)
	{
		wchar_t    *workspace;
		size_t		curr_char;

		/* Overflow paranoia */
		if ((srclen + 1) > (INT_MAX / sizeof(wchar_t)))
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		/* Output workspace cannot have more codes than input bytes */
		workspace = (wchar_t *) palloc((srclen + 1) * sizeof(wchar_t));

		char2wchar(workspace, srclen + 1, src, srclen, locale);

		for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
			workspace[curr_char] = towupper_l(workspace[curr_char], loc);

		/*
		 * Make result large enough; case change might change number of bytes
		 */
		result_size = curr_char * pg_database_encoding_max_length();
		if (result_size + 1 > destsize)
			return result_size;

		wchar2char(dest, workspace, result_size + 1, locale);
		pfree(workspace);
	}
	else
	{
		char	   *p;

		result_size = srclen;
		if (result_size + 1 > destsize)
			return result_size;

		strlcpy(dest, src, srclen + 1);

		/*
		 * Note: we assume that toupper_l() will not be so broken as to need
		 * an islower_l() guard test.  When using the default collation, we
		 * apply the traditional Postgres behavior that forces ASCII-style
		 * treatment of I/i, but in non-default collations you get exactly
		 * what the collation says.
		 */
		for (p = dest; *p; p++)
			*p = toupper_l((unsigned char) *p, loc);
	}

	result_size = strlen(dest);
	return result_size;
}

pg_locale_t
dat_create_locale_libc(HeapTuple dattuple)
{
	Form_pg_database dbform;
	Datum		datum;
	const char *datcollate;
	const char *datctype;
	locale_t	loc;
	pg_locale_t result;

	dbform = (Form_pg_database) GETSTRUCT(dattuple);

	datum = SysCacheGetAttrNotNull(DATABASEOID, dattuple,
								   Anum_pg_database_datcollate);
	datcollate = TextDatumGetCString(datum);

	datum = SysCacheGetAttrNotNull(DATABASEOID, dattuple,
								   Anum_pg_database_datctype);
	datctype = TextDatumGetCString(datum);

	loc = make_libc_collator(datcollate, datctype);

	result = MemoryContextAllocZero(TopMemoryContext,
									sizeof(struct pg_locale_struct));
	result->provider = dbform->datlocprovider;
	result->deterministic = true;
	result->collate_is_c = (strcmp(datcollate, "C") == 0) ||
		(strcmp(datcollate, "POSIX") == 0);
	result->ctype_is_c = (strcmp(datctype, "C") == 0) ||
		(strcmp(datctype, "POSIX") == 0);
	result->info.lt = loc;
	if (!result->collate_is_c)
		result->collate = &collate_methods_libc;
	if (!result->ctype_is_c)
		result->casemap = &casemap_methods_libc;

	return result;
}

pg_locale_t
coll_create_locale_libc(HeapTuple colltuple, MemoryContext context)
{
	Form_pg_collation collform;
	Datum		datum;
	const char *collcollate;
	const char *collctype;
	locale_t	loc;
	pg_locale_t result;

	collform = (Form_pg_collation) GETSTRUCT(colltuple);

	datum = SysCacheGetAttrNotNull(COLLOID, colltuple,
								   Anum_pg_collation_collcollate);
	collcollate = TextDatumGetCString(datum);
	datum = SysCacheGetAttrNotNull(COLLOID, colltuple,
								   Anum_pg_collation_collctype);
	collctype = TextDatumGetCString(datum);

	loc = make_libc_collator(collcollate, collctype);

	result = MemoryContextAllocZero(context, sizeof(struct pg_locale_struct));
	result->provider = collform->collprovider;
	result->deterministic = collform->collisdeterministic;
	result->collate_is_c = (strcmp(collcollate, "C") == 0) ||
		(strcmp(collcollate, "POSIX") == 0);
	result->ctype_is_c = (strcmp(collctype, "C") == 0) ||
		(strcmp(collctype, "POSIX") == 0);
	result->info.lt = loc;
	if (!result->collate_is_c)
		result->collate = &collate_methods_libc;
	if (!result->ctype_is_c)
		result->casemap = &casemap_methods_libc;

	return result;
}

/*
 * Create a locale_t with the given collation and ctype.
 *
 * The "C" and "POSIX" locales are not actually handled by libc, so return
 * NULL.
 *
 * Ensure that no path leaks a locale_t.
 */
static locale_t
make_libc_collator(const char *collate, const char *ctype)
{
	locale_t	loc = 0;

	if (strcmp(collate, ctype) == 0)
	{
		if (strcmp(ctype, "C") != 0 && strcmp(ctype, "POSIX") != 0)
		{
			/* Normal case where they're the same */
			errno = 0;
#ifndef WIN32
			loc = newlocale(LC_COLLATE_MASK | LC_CTYPE_MASK, collate,
							NULL);
#else
			loc = _create_locale(LC_ALL, collate);
#endif
			if (!loc)
				report_newlocale_failure(collate);
		}
	}
	else
	{
#ifndef WIN32
		/* We need two newlocale() steps */
		locale_t	loc1 = 0;

		if (strcmp(collate, "C") != 0 && strcmp(collate, "POSIX") != 0)
		{
			errno = 0;
			loc1 = newlocale(LC_COLLATE_MASK, collate, NULL);
			if (!loc1)
				report_newlocale_failure(collate);
		}

		if (strcmp(ctype, "C") != 0 && strcmp(ctype, "POSIX") != 0)
		{
			errno = 0;
			loc = newlocale(LC_CTYPE_MASK, ctype, loc1);
			if (!loc)
			{
				if (loc1)
					freelocale(loc1);
				report_newlocale_failure(ctype);
			}
		}
		else
			loc = loc1;
#else

		/*
		 * XXX The _create_locale() API doesn't appear to support this. Could
		 * perhaps be worked around by changing pg_locale_t to contain two
		 * separate fields.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("collations with different collate and ctype values are not supported on this platform")));
#endif
	}

	return loc;
}

/*
 * strncoll_libc
 *
 * NUL-terminate arguments, if necessary, and pass to strcoll_l().
 *
 * An input string length of -1 means that it's already NUL-terminated.
 */
int
strncoll_libc(const char *arg1, ssize_t len1, const char *arg2, ssize_t len2,
			  pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	size_t		bufsize1 = (len1 == -1) ? 0 : len1 + 1;
	size_t		bufsize2 = (len2 == -1) ? 0 : len2 + 1;
	const char *arg1n;
	const char *arg2n;
	int			result;

	Assert(locale->provider == COLLPROVIDER_LIBC);

#ifdef WIN32
	/* check for this case before doing the work for nul-termination */
	if (GetDatabaseEncoding() == PG_UTF8)
		return strncoll_libc_win32_utf8(arg1, len1, arg2, len2, locale);
#endif							/* WIN32 */

	if (bufsize1 + bufsize2 > TEXTBUFLEN)
		buf = palloc(bufsize1 + bufsize2);

	/* nul-terminate arguments if necessary */
	if (len1 == -1)
	{
		arg1n = arg1;
	}
	else
	{
		char	   *buf1 = buf;

		memcpy(buf1, arg1, len1);
		buf1[len1] = '\0';
		arg1n = buf1;
	}

	if (len2 == -1)
	{
		arg2n = arg2;
	}
	else
	{
		char	   *buf2 = buf + bufsize1;

		memcpy(buf2, arg2, len2);
		buf2[len2] = '\0';
		arg2n = buf2;
	}

	result = strcoll_l(arg1n, arg2n, locale->info.lt);

	if (buf != sbuf)
		pfree(buf);

	return result;
}

/*
 * strnxfrm_libc
 *
 * NUL-terminate src, if necessary, and pass to strxfrm_l().
 *
 * A source length of -1 means that it's already NUL-terminated.
 */
size_t
strnxfrm_libc(char *dest, size_t destsize, const char *src, ssize_t srclen,
			  pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	size_t		bufsize = srclen + 1;
	size_t		result;

	Assert(locale->provider == COLLPROVIDER_LIBC);

	if (srclen == -1)
		return strxfrm_l(dest, src, destsize, locale->info.lt);

	if (bufsize > TEXTBUFLEN)
		buf = palloc(bufsize);

	/* nul-terminate argument */
	memcpy(buf, src, srclen);
	buf[srclen] = '\0';

	result = strxfrm_l(dest, buf, destsize, locale->info.lt);

	if (buf != sbuf)
		pfree(buf);

	/* if dest is defined, it should be nul-terminated */
	Assert(result >= destsize || dest[result] == '\0');

	return result;
}

/*
 * strncoll_libc_win32_utf8
 *
 * Win32 does not have UTF-8. Convert UTF8 arguments to wide characters and
 * invoke wcscoll_l().
 *
 * An input string length of -1 means that it's NUL-terminated.
 */
#ifdef WIN32
static int
strncoll_libc_win32_utf8(const char *arg1, ssize_t len1, const char *arg2,
						 ssize_t len2, pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	char	   *a1p,
			   *a2p;
	int			a1len;
	int			a2len;
	int			r;
	int			result;

	Assert(locale->provider == COLLPROVIDER_LIBC);
	Assert(GetDatabaseEncoding() == PG_UTF8);

	if (len1 == -1)
		len1 = strlen(arg1);
	if (len2 == -1)
		len2 = strlen(arg2);

	a1len = len1 * 2 + 2;
	a2len = len2 * 2 + 2;

	if (a1len + a2len > TEXTBUFLEN)
		buf = palloc(a1len + a2len);

	a1p = buf;
	a2p = buf + a1len;

	/* API does not work for zero-length input */
	if (len1 == 0)
		r = 0;
	else
	{
		r = MultiByteToWideChar(CP_UTF8, 0, arg1, len1,
								(LPWSTR) a1p, a1len / 2);
		if (!r)
			ereport(ERROR,
					(errmsg("could not convert string to UTF-16: error code %lu",
							GetLastError())));
	}
	((LPWSTR) a1p)[r] = 0;

	if (len2 == 0)
		r = 0;
	else
	{
		r = MultiByteToWideChar(CP_UTF8, 0, arg2, len2,
								(LPWSTR) a2p, a2len / 2);
		if (!r)
			ereport(ERROR,
					(errmsg("could not convert string to UTF-16: error code %lu",
							GetLastError())));
	}
	((LPWSTR) a2p)[r] = 0;

	errno = 0;
	result = wcscoll_l((LPWSTR) a1p, (LPWSTR) a2p, locale->info.lt);
	if (result == 2147483647)	/* _NLSCMPERROR; missing from mingw headers */
		ereport(ERROR,
				(errmsg("could not compare Unicode strings: %m")));

	if (buf != sbuf)
		pfree(buf);

	return result;
}
#endif							/* WIN32 */

/* simple subroutine for reporting errors from newlocale() */
static void
report_newlocale_failure(const char *localename)
{
	int			save_errno;

	/*
	 * Windows doesn't provide any useful error indication from
	 * _create_locale(), and BSD-derived platforms don't seem to feel they
	 * need to set errno either (even though POSIX is pretty clear that
	 * newlocale should do so).  So, if errno hasn't been set, assume ENOENT
	 * is what to report.
	 */
	if (errno == 0)
		errno = ENOENT;

	/*
	 * ENOENT means "no such locale", not "no such file", so clarify that
	 * errno with an errdetail message.
	 */
	save_errno = errno;			/* auxiliary funcs might change errno */
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("could not create locale \"%s\": %m",
					localename),
			 (save_errno == ENOENT ?
			  errdetail("The operating system could not find any locale data for the locale name \"%s\".",
						localename) : 0)));
}
