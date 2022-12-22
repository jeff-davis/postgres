/*--------------------------------------------------------------------------
 *
 * test_icu_hook.c
 *		Code for testing collation provider icu hook.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/test_collation_lib_hooks/test_icu_hook.c
 *
 * Implements a custom icu-like collation provider library for testing the
 * hooks. It accepts any collation name requested. All behave exactly like the
 * "en_US" locale, except for the locale named "DESC", which reverses the sort
 * order and reverses uppercase/lowercase behavior.
 *
 * The version is always reported as 2.72, so loading it will cause a version
 * mismatch warning.
 *
 * -------------------------------------------------------------------------
 */

#include "test_collation_lib_hooks.h"

#ifdef USE_ICU

#include <unicode/ucnv.h>
#include <unicode/ulocdata.h>
#include <unicode/ustring.h>

#define TEST_LOCALE "en_US"

typedef struct TestUCollator {
	UCollator	*ucol;
	bool		 reverse;
} TestUCollator;

static pg_icu_library *test_icu_library = NULL;
static const UVersionInfo test_icu_version = { 2, 72 };

static bool
locale_is_reverse(const char *locale)
{
	if (strcmp(locale, "DESC") == 0)
		return true;
	else
		return false;
}

static UCollator *
test_openCollator(const char *loc, UErrorCode *status)
{
	TestUCollator *testcol = MemoryContextAlloc(TopMemoryContext, sizeof(TestUCollator));
	UCollator *ucol = ucol_open(TEST_LOCALE, status);
	testcol->ucol = ucol;
	testcol->reverse = locale_is_reverse(loc);
	return (UCollator *)testcol;
}

static void
test_closeCollator(UCollator *coll)
{
	TestUCollator *testcol = (TestUCollator *) coll;
	ucol_close(testcol->ucol);
	pfree(testcol);
}

static void
test_setAttribute(UCollator *coll, UColAttribute attr,
				  UColAttributeValue value, UErrorCode *status)
{
	TestUCollator *testcol = (TestUCollator *) coll;
	ucol_setAttribute(testcol->ucol, attr, value, status);
}

static void
test_getCollatorVersion(const UCollator *coll, UVersionInfo info)
{
	memcpy(info, test_icu_version, sizeof(UVersionInfo));
}

static UCollationResult
test_strcoll(const UCollator *coll, const UChar *source, int32_t sourceLength,
			 const UChar *target, int32_t targetLength)
{
	TestUCollator *testcol = (TestUCollator *) coll;
	UCollationResult ret = ucol_strcoll(testcol->ucol, source, sourceLength,
										target, targetLength);
	if (testcol->reverse)
		return -ret;
	else
		return ret;
}

static UCollationResult
test_strcollUTF8(const UCollator *coll, const char *source,
				 int32_t sourceLength, const char *target,
				 int32_t targetLength, UErrorCode *status)
{
	TestUCollator *testcol = (TestUCollator *) coll;
	UCollationResult ret = ucol_strcollUTF8(testcol->ucol, source,
											sourceLength, target,
											targetLength, status);
	if (testcol->reverse)
		return -ret;
	else
		return ret;
}

static int32_t
test_getSortKey(const UCollator *coll, const UChar *source,
				int32_t sourceLength, uint8_t *result, int32_t resultLength)
{
	TestUCollator *testcol = (TestUCollator *) coll;
	int32_t ret = ucol_getSortKey(testcol->ucol, source, sourceLength,
								  result, resultLength);
	size_t result_size = ret + 1;

	if (resultLength >= result_size)
	{
		result[resultLength] = '\0';

		if (testcol->reverse)
			for (int i = 0; i < result_size; i++)
				*((unsigned char *) result + i) ^= (unsigned char) 0xff;
	}

	return result_size;
}

static int32_t
test_nextSortKeyPart(const UCollator *coll, UCharIterator *iter,
					 uint32_t state[2], uint8_t *dest, int32_t count,
					 UErrorCode *status)
{
	TestUCollator *testcol = (TestUCollator *) coll;
	int32_t ret = ucol_nextSortKeyPart(testcol->ucol, iter, state, dest,
									   count, status);

	if (testcol->reverse)
		for (int i = 0; i < ret; i++)
			*((unsigned char *) dest + i) ^= (unsigned char) 0xff;

	/*
	 * The following is not correct for cases where we finish precisely on the
	 * boundary (i.e. count is exactly enough). To fix this we'd need to track
	 * additional state across calls, which doesn't seem worth it for a test
	 * case.
	 */
	if (count >= ret && ret > 0)
	{
		if (testcol->reverse)
			dest[ret] = 0xff;
		else
			dest[ret] = '\0';
		return ret + 1;
	}

	return ret;
}

static int32_t
test_strToUpper(UChar *dest, int32_t destCapacity, const UChar *src,
				int32_t srcLength, const char *locale, UErrorCode *pErrorCode)
{
	if (locale_is_reverse(locale))
		return u_strToLower(dest, destCapacity, src, srcLength,
							TEST_LOCALE, pErrorCode);
	else
		return u_strToUpper(dest, destCapacity, src, srcLength,
							TEST_LOCALE, pErrorCode);
}

static int32_t
test_strToLower(UChar *dest, int32_t destCapacity, const UChar *src,
				int32_t srcLength, const char *locale, UErrorCode *pErrorCode)
{
	if (locale_is_reverse(locale))
		return u_strToUpper(dest, destCapacity, src, srcLength,
							TEST_LOCALE, pErrorCode);
	else
		return u_strToLower(dest, destCapacity, src, srcLength,
							TEST_LOCALE, pErrorCode);
}

pg_icu_library *
test_get_icu_library(const char *locale, const char *version)
{
	pg_icu_library *lib;

	if (test_icu_library != NULL)
		return test_icu_library;

	ereport(LOG, (errmsg("loading custom ICU provider for test_collation_lib_hooks")));

	lib = MemoryContextAlloc(TopMemoryContext, sizeof(pg_icu_library));
	lib->getICUVersion = u_getVersion;
	lib->getUnicodeVersion = u_getUnicodeVersion;
	lib->getCLDRVersion = ulocdata_getCLDRVersion;
	lib->openCollator = test_openCollator;
	lib->closeCollator = test_closeCollator;
	lib->getCollatorVersion = test_getCollatorVersion;
	lib->getUCAVersion = ucol_getUCAVersion;
	lib->versionToString = u_versionToString;
	lib->strcoll = test_strcoll;
	lib->strcollUTF8 = test_strcollUTF8;
	lib->getSortKey = test_getSortKey;
	lib->nextSortKeyPart = test_nextSortKeyPart;
	lib->setUTF8 = uiter_setUTF8;
	lib->errorName = u_errorName;
	lib->strToUpper = test_strToUpper;
	lib->strToLower = test_strToLower;
	lib->strToTitle = u_strToTitle;
	lib->setAttribute = test_setAttribute;
	lib->openConverter = ucnv_open;
	lib->closeConverter = ucnv_close;
	lib->fromUChars = ucnv_fromUChars;
	lib->toUChars = ucnv_toUChars;
	lib->toLanguageTag = uloc_toLanguageTag;
	lib->getDisplayName = uloc_getDisplayName;
	lib->countAvailable = uloc_countAvailable;
	lib->getAvailable = uloc_getAvailable;

	test_icu_library = lib;
	return lib;
}

#endif				/* USE_ICU */
