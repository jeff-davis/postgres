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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_ICU
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#endif
#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "common/unicode_version.h"

#ifdef USE_ICU
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
	UChar ubuf_orig[PG_U_MAX_CASE_EXPANSION * 2];
	UChar ubuf_mapped[PG_U_MAX_CASE_EXPANSION * 2];
	pg_wchar icu_special[PG_U_MAX_CASE_EXPANSION] = {0};
	UErrorCode status;

	status = U_ZERO_ERROR;
	u_strFromUTF32(ubuf_orig, 16, &ubuf_orig_len, (UChar32 *)&code, 1,
				   &status);
	if (U_FAILURE(status))
	{
		printf("error testing codepoint %06x: could not convert from UTF32: %s\n",
			   code, u_errorName(status));
		exit(1);
	}

	status = U_ZERO_ERROR;
	ubuf_mapped_len = func(ubuf_mapped, 16, ubuf_orig, ubuf_orig_len, "und",
						   &status);
	if (U_FAILURE(status))
	{
		printf("error converting codepoint %06x to %scase: %s\n",
			   code, errstr, u_errorName(status));
		exit(1);
	}

	status = U_ZERO_ERROR;
	u_strToUTF32((UChar32 *)icu_special, PG_U_MAX_CASE_EXPANSION, NULL,
				 ubuf_mapped, ubuf_mapped_len, &status);
	if (U_FAILURE(status))
	{
		printf("error testing codepoint %06x: could not convert to UTF32: %s\n",
			   code, u_errorName(status));
		exit(1);
	}

	if (memcmp(expected, icu_special,
			   sizeof(pg_wchar) * PG_U_MAX_CASE_EXPANSION) != 0)
	{
		printf("FAILURE for codepoint %06x\n", code);
		printf("Postgres special %scase mapping: %06x %06x %06x\n", errstr,
			   expected[0], expected[1], expected[2]);
		printf("ICU special %scase mapping: %06x %06x %06x\n", errstr,
			   icu_special[0], icu_special[1], icu_special[2]);
		exit(1);
	}
}

static void
test_special(pg_wchar code)
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
		printf("Skipping code point %06x.\n", code);
	else
		icu_special(code, title_special, u_strToTitle_default_BI, "title");

	icu_special(code, upper_special, u_strToUpper, "upper");
}

static void
test_simple(pg_wchar code)
{
	const pg_wchar *lower_special;
	const pg_wchar *upper_special;
	const pg_wchar *title_special;
	pg_wchar lower = unicode_lowercase(code, &lower_special);
	pg_wchar title = unicode_titlecase(code, &title_special);
	pg_wchar upper = unicode_uppercase(code, &upper_special);
	pg_wchar iculower = u_tolower(code);
	pg_wchar icutitle = u_totitle(code);
	pg_wchar icuupper = u_toupper(code);

	if (lower != iculower || title != icutitle || upper != icuupper)
	{
		printf("FAILURE for codepoint %06x\n", code);
		printf("Postgres lower/title/upper:	%06x/%06x/%06x\n",
			   lower, title, upper);
		printf("ICU lower/title/upper:		%06x/%06x/%06x\n",
			   iculower, icutitle, icuupper);
		printf("\n");
		exit(1);
	}
}
#endif

/*
 * Exhaustively test that the Unicode simple and special case mapping for each
 * codepoint matches that returned by ICU.
 */
int
main(int argc, char **argv)
{
#ifdef USE_ICU
	printf("Postgres Unicode Version:\t%s\n", PG_UNICODE_VERSION);
	printf("ICU Unicode Version:\t\t%s\n", U_UNICODE_VERSION);

	for (pg_wchar code = 0; code <= 0x10ffff; code++)
	{
		pg_unicode_category category = unicode_category(code);
		if (category != PG_U_UNASSIGNED && category != PG_U_SURROGATE)
			test_simple(code);
	}
	printf("case_test: simple mapping test successful\n");

	for (pg_wchar code = 0; code <= 0x10ffff; code++)
	{
		pg_unicode_category category = unicode_category(code);
		if (category != PG_U_UNASSIGNED && category != PG_U_SURROGATE)
			test_special(code);
	}
	printf("case_test: special mapping test successful\n");

	printf("case_test: All tests successful!\n");
	exit(0);
#else
	printf("ICU support required for test; skipping.\n");
	exit(0);
#endif
}
