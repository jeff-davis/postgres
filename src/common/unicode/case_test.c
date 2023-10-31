/*-------------------------------------------------------------------------
 * case_test.c
 *		Program to test Unicode case mapping functions.
 *
 * Portions Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/unicode/case_test.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#ifdef USE_ICU
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#endif
#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "common/unicode_version.h"

/*
 * We expect that C.UTF-8 has the same CTYPE behavior as the simple unicode
 * mappings, but that's not guaranteed. If there are failures in the libc
 * test, that's useful information, but does not necessarily indicate a
 * problem.
 */
#define LIBC_LOCALE "C.UTF-8"

#ifdef USE_ICU

/* use root locale for test */
#define ICU_LOCALE "und"

typedef int32_t (*ICU_Convert_Func) (UChar *dest, int32_t destCapacity,
									 const UChar *src, int32_t srcLength,
									 const char *locale,
									 UErrorCode *pErrorCode);

static int32_t
u_strToTitle_default_BI(UChar *dest, int32_t destCapacity,
						const UChar *src, int32_t srcLength,
						const char *locale,
						UErrorCode *pErrorCode)
{
	return u_strToTitle(dest, destCapacity, src, srcLength,
						NULL, locale, pErrorCode);
}

static void
icu_special(pg_wchar code, const pg_wchar expected[3], ICU_Convert_Func func, const char *errstr)
{
	int32 ubuf_orig_len;
	int32 ubuf_mapped_len;
	UChar ubuf_orig[2]; /* up to two UChars per UChar32 */
	UChar ubuf_mapped[PG_U_MAX_CASE_EXPANSION * 2];
	pg_wchar icu_special[PG_U_MAX_CASE_EXPANSION] = {0};
	UErrorCode status;

	status = U_ZERO_ERROR;
	u_strFromUTF32(ubuf_orig, 2, &ubuf_orig_len, (UChar32 *)&code, 1,
				   &status);
	if (U_FAILURE(status))
	{
		printf("case_test: error testing codepoint 0x%06x: could not convert from UTF32: %s\n",
			   code, u_errorName(status));
		exit(1);
	}

	status = U_ZERO_ERROR;
	ubuf_mapped_len = func(ubuf_mapped, PG_U_MAX_CASE_EXPANSION * 2,
						   ubuf_orig, ubuf_orig_len, ICU_LOCALE, &status);
	if (U_FAILURE(status))
	{
		printf("case_test: error converting codepoint 0x%06x to %scase: %s\n",
			   code, errstr, u_errorName(status));
		exit(1);
	}

	status = U_ZERO_ERROR;
	u_strToUTF32((UChar32 *)icu_special, PG_U_MAX_CASE_EXPANSION, NULL,
				 ubuf_mapped, ubuf_mapped_len, &status);
	if (U_FAILURE(status))
	{
		printf("case_test: error testing codepoint 0x%06x: could not convert to UTF32: %s\n",
			   code, u_errorName(status));
		exit(1);
	}

	if (memcmp(expected, icu_special,
			   sizeof(pg_wchar) * PG_U_MAX_CASE_EXPANSION) != 0)
	{
		printf("case_test: FAILURE for codepoint 0x%06x\n", code);
		printf("case_test: Postgres special %scase mapping:	0x%06x 0x%06x 0x%06x\n", errstr,
			   expected[0], expected[1], expected[2]);
		printf("case_test: ICU special %scase mapping:	0x%06x 0x%06x 0x%06x\n", errstr,
			   icu_special[0], icu_special[1], icu_special[2]);
		exit(1);
	}
}

static void
icu_test_special(pg_wchar code)
{
	const pg_wchar *lower_special;
	const pg_wchar *upper_special;
	const pg_wchar *title_special;
	pg_wchar lower = unicode_lowercase(code, &lower_special);
	pg_wchar title = unicode_titlecase(code, &title_special);
	pg_wchar upper = unicode_uppercase(code, &upper_special);
	pg_wchar lower_buf[PG_U_MAX_CASE_EXPANSION] = {0};
	pg_wchar title_buf[PG_U_MAX_CASE_EXPANSION] = {0};
	pg_wchar upper_buf[PG_U_MAX_CASE_EXPANSION] = {0};

	lower_buf[0] = lower;
	title_buf[0] = title;
	upper_buf[0] = upper;
	if (!lower_special)
		lower_special = lower_buf;
	if (!title_special)
		title_special = title_buf;
	if (!upper_special)
		upper_special = upper_buf;

	icu_special(code, lower_special, u_strToLower, "lower");

	/*
	 * XXX: In ICU, the following character ("COMBINING GREEK YPOGEGRAMMENI")
	 * is not titlecased as expected using u_strToTitle(). Skip for now.
	 */
	if (code == 0x000345)
		printf("case_test: ICU special: skipping code point 0x%06x\n", code);
	else
		icu_special(code, title_special, u_strToTitle_default_BI, "title");

	icu_special(code, upper_special, u_strToUpper, "upper");
}

static void
icu_test_simple(pg_wchar code)
{
	pg_wchar lower = unicode_lowercase(code, NULL);
	pg_wchar title = unicode_titlecase(code, NULL);
	pg_wchar upper = unicode_uppercase(code, NULL);
	pg_wchar iculower = u_tolower(code);
	pg_wchar icutitle = u_totitle(code);
	pg_wchar icuupper = u_toupper(code);

	if (lower != iculower || title != icutitle || upper != icuupper)
	{
		printf("case_test: FAILURE for codepoint 0x%06x\n", code);
		printf("case_test: Postgres lower/title/upper:	0x%06x/0x%06x/0x%06x\n",
			   lower, title, upper);
		printf("case_test: ICU lower/title/upper:		0x%06x/0x%06x/0x%06x\n",
			   iculower, icutitle, icuupper);
		printf("\n");
		exit(1);
	}
}
#endif

static void
libc_test_simple(pg_wchar code)
{
	pg_wchar lower = unicode_lowercase(code, NULL);
	pg_wchar upper = unicode_uppercase(code, NULL);
	wchar_t libclower = towlower(code);
	wchar_t libcupper = towupper(code);

	if (lower != libclower || upper != libcupper)
	{
		printf("case_test: FAILURE for codepoint 0x%06x\n", code);
		printf("case_test: Postgres lower/upper:	0x%06x/0x%06x\n",
			   lower, upper);
		printf("case_test: libc lower/upper:		0x%06x/0x%06x\n",
			   libclower, libcupper);
		printf("\n");
		exit(1);
	}
}

/*
 * Exhaustively compare case mappings with the results from libc and ICU.
 */
int
main(int argc, char **argv)
{
	char *libc_locale;

	libc_locale = setlocale(LC_CTYPE, LIBC_LOCALE);

	printf("case_test: Postgres Unicode version:\t%s\n", PG_UNICODE_VERSION);
#ifdef USE_ICU
	printf("case_test: ICU Unicode version:\t\t%s\n", U_UNICODE_VERSION);
#else
	printf("case_test: ICU not available; skipping\n");
#endif

	if (libc_locale)
	{
		printf("case_test: comparing with libc locale \"%s\"\n", libc_locale);
		for (pg_wchar code = 0; code <= 0x10ffff; code++)
		{
			pg_unicode_category category = unicode_category(code);
			if (category != PG_U_UNASSIGNED && category != PG_U_SURROGATE)
				libc_test_simple(code);
		}
		printf("case_test: libc simple mapping test successful\n");
	}
	else
		printf("case_test: libc locale \"%s\" not available; skipping\n",
			   LIBC_LOCALE);

#ifdef USE_ICU
	for (pg_wchar code = 0; code <= 0x10ffff; code++)
	{
		pg_unicode_category category = unicode_category(code);
		if (category != PG_U_UNASSIGNED && category != PG_U_SURROGATE)
			icu_test_simple(code);
	}
	printf("case_test: ICU simple mapping test successful\n");

	for (pg_wchar code = 0; code <= 0x10ffff; code++)
	{
		pg_unicode_category category = unicode_category(code);
		if (category != PG_U_UNASSIGNED && category != PG_U_SURROGATE)
			icu_test_special(code);
	}
	printf("case_test: ICU special mapping test successful\n");
#endif

	exit(0);
}
