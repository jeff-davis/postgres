/*-------------------------------------------------------------------------
 * category_test.c
 *		Program to test Unicode general category functions.
 *
 * Portions Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/unicode/category_test.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef USE_ICU
#include <unicode/uchar.h>
#endif

#include "common/unicode_version.h"
#include "common/unicode_category.h"

static int pg_unicode_version = 0;
static int icu_unicode_version = 0;

#ifdef USE_ICU
/*
 * Parse version into integer for easy comparison.
 */
static int
parse_unicode_version(const char *version)
{
	int			n,
				major,
				minor;

	n = sscanf(version, "%d.%d", &major, &minor);

	Assert(n == 2);
	Assert(minor < 100);

	return major * 100 + minor;
}

static void
icu_test()
{
	int		 pg_skipped_codepoints	= 0;
	int		 icu_skipped_codepoints = 0;

	for (pg_wchar code = 0; code <= 0x10ffff; code++)
	{
		uint8_t	pg_category	  = unicode_category(code);
		uint8_t	icu_category  = u_charType(code);
		bool	hasalpha	  = unicode_is_alphabetic(code);
		bool	haslower	  = unicode_is_lowercase(code);
		bool	hasupper	  = unicode_is_uppercase(code);
		bool	hasspace	  = unicode_is_white_space(code);
		bool	hasxdigit	  = unicode_is_hex_digit(code);
		bool	icu_hasalpha  = u_hasBinaryProperty(code, UCHAR_ALPHABETIC);
		bool	icu_haslower  = u_hasBinaryProperty(code, UCHAR_LOWERCASE);
		bool	icu_hasupper  = u_hasBinaryProperty(code, UCHAR_UPPERCASE);
		bool	icu_hasspace  = u_hasBinaryProperty(code, UCHAR_WHITE_SPACE);
		bool	icu_hasxdigit = u_hasBinaryProperty(code, UCHAR_HEX_DIGIT);

		/*
		 * A version mismatch means that some assigned codepoints in the
		 * newer version may be unassigned in the older version. That's
		 * OK, though the test will not cover those codepoints marked
		 * unassigned in the older version (that is, it will no longer be
		 * an exhaustive test).
		 */
		if (pg_category == PG_U_UNASSIGNED &&
			icu_category != PG_U_UNASSIGNED &&
			pg_unicode_version < icu_unicode_version)
		{
			pg_skipped_codepoints++;
			continue;
		}

		if (icu_category == PG_U_UNASSIGNED &&
			pg_category != PG_U_UNASSIGNED &&
			icu_unicode_version < pg_unicode_version)
		{
			icu_skipped_codepoints++;
			continue;
		}

		if (pg_category != icu_category)
		{
			printf("category_test: FAILURE for codepoint 0x%06x\n", code);
			printf("category_test: Postgres category:	%02d %s %s\n", pg_category,
				   unicode_category_abbrev(pg_category),
				   unicode_category_string(pg_category));
			printf("category_test: ICU category:		%02d %s %s\n", icu_category,
				   unicode_category_abbrev(icu_category),
				   unicode_category_string(icu_category));
			printf("\n");
			exit(1);
		}

		if (hasalpha != icu_hasalpha || haslower != icu_haslower ||
			hasupper != icu_hasupper || hasspace != icu_hasspace ||
			hasxdigit != icu_hasxdigit)
		{
			printf("category_test: FAILURE for codepoint 0x%06x\n", code);
			printf("category_test: Postgres	property	alpha/lower/upper/space/xdigit: %d/%d/%d/%d/%d\n",
				   hasalpha, haslower, hasupper, hasspace, hasxdigit);
			printf("category_test: ICU property		alpha/lower/upper/space/xdigit: %d/%d/%d/%d/%d\n",
				   icu_hasalpha, icu_haslower, icu_hasupper, icu_hasspace, icu_hasxdigit);
			printf("\n");
			exit(1);
		}
	}

	if (pg_skipped_codepoints > 0)
		printf("category_test: skipped %d codepoints unassigned in Postgres due to Unicode version mismatch\n",
			   pg_skipped_codepoints);
	if (icu_skipped_codepoints > 0)
		printf("category_test: skipped %d codepoints unassigned in ICU due to Unicode version mismatch\n",
			   icu_skipped_codepoints);

	printf("category_test: ICU test successful\n");
}
#endif

/*
 * Exhaustively test that the Unicode category for each codepoint matches that
 * returned by ICU.
 */
int
main(int argc, char **argv)
{
#ifdef USE_ICU
	pg_unicode_version = parse_unicode_version(PG_UNICODE_VERSION);
	printf("category_test: Postgres Unicode version:\t%s\n", PG_UNICODE_VERSION);

	icu_unicode_version = parse_unicode_version(U_UNICODE_VERSION);
	printf("category_test: ICU Unicode version:\t\t%s\n", U_UNICODE_VERSION);
	icu_test();
#else
	printf("category_test: ICU not available; skipping\n");
#endif
}
