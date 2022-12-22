/*--------------------------------------------------------------------------
 *
 * test_libc_hook.c
 *		Code for testing collation provider libc hook.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/test_collation_lib_hooks/test_libc_hook.c
 *
 * Implements a custom libc-like collation provider library for testing the
 * hooks. It accepts any collation name requested. All behave exactly like the
 * "C" locale, except for the locale named "DESC", which reverses the sort
 * order and reverses uppercase/lowercase behavior.
 *
 * The version is always reported as 3.14159, so loading it will cause a
 * version mismatch warning.
 *
 * -------------------------------------------------------------------------
 */

#include "test_collation_lib_hooks.h"

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

#ifdef WIN32
#include <shlwapi.h>
#endif

#define LOCALE_NAME_LEN 64

typedef struct test_locale_t
{
	bool reverse_sort;
	bool reverse_case;
	char lc_collate[LOCALE_NAME_LEN];
	char lc_ctype[LOCALE_NAME_LEN];
} test_locale_t;

static pg_libc_library *test_libc_library = NULL;
static test_locale_t current_setlocale = {
	.lc_collate = "C",
	.lc_ctype = "C",
	.reverse_sort = false,
	.reverse_case = false
};

#ifdef HAVE_LOCALE_T
#ifndef WIN32
static test_locale_t *current_uselocale = &current_setlocale;
#endif
#endif

#ifdef HAVE_LOCALE_T
static locale_t c_locale_t = NULL;
#endif

void
init_libc_hook()
{
#ifdef HAVE_LOCALE_T
#ifndef WIN32
	c_locale_t = newlocale(LC_ALL_MASK, "C", NULL);
#else
	c_locale_t = _create_locale(LC_ALL, "C");
#endif
#endif
}

#ifdef HAVE_LOCALE_T
static test_locale_t *
current_locale(void)
{
#ifndef WIN32
	return current_uselocale;
#else
	return &current_setlocale;
#endif
}
#endif

static bool
locale_is_reverse(const char *locale)
{
	if (strcmp(locale, "DESC") == 0)
		return true;
	else
		return false;
}

static const char *
test_libc_version(void)
{
	return "3.14159";
}

#ifdef WIN32
bool
test_GetNLSVersionEx(NLS_FUNCTION function, LPCWSTR lpLocaleName,
					 LPNLSVERSIONINFOEX lpVersionInformation)
{
	Assert(function == COMPARE_STRING);
	if (wcscmp(lpLocaleName, L"DESC") == 0)
	{
		lpVersionInformation->dwNLSVersion = (6 << 8) | 28;
		lpVersionInformation->dwDefinedVersion = (6 << 8) | 28;
	}
	else
	{
		lpVersionInformation->dwNLSVersion = (3 << 8) | 14;
		lpVersionInformation->dwDefinedVersion = (3 << 8) | 14;
	}

	return true;
}
#endif

static char *
test_setlocale(int category, const char *locale)
{
	Assert (category == LC_COLLATE || category == LC_CTYPE ||
			category == LC_ALL);

	if (category == LC_ALL)
	{
		if (locale)
		{
			current_setlocale.reverse_sort = locale_is_reverse(locale);
			current_setlocale.reverse_case = locale_is_reverse(locale);
			strncpy(current_setlocale.lc_collate, locale, LOCALE_NAME_LEN);
			strncpy(current_setlocale.lc_ctype, locale, LOCALE_NAME_LEN);
		}
		return current_setlocale.lc_collate;
	}
	else if (category == LC_COLLATE)
	{
		if (locale)
		{
			current_setlocale.reverse_sort = locale_is_reverse(locale);
			strncpy(current_setlocale.lc_collate, locale, LOCALE_NAME_LEN);
		}

		return current_setlocale.lc_collate;
	}
	else if (category == LC_CTYPE)
	{
		if (locale)
		{
			current_setlocale.reverse_case = locale_is_reverse(locale);
			strncpy(current_setlocale.lc_ctype, locale, LOCALE_NAME_LEN);
		}

		return current_setlocale.lc_ctype;
	}

	return NULL;
}

#ifdef HAVE_LOCALE_T

#ifndef WIN32

static locale_t
test_newlocale(int category, const char *locale, locale_t baselocale_t)
{
	test_locale_t *newloc;

	Assert(baselocale_t != LC_GLOBAL_LOCALE);
	Assert((test_locale_t *) baselocale_t != &current_setlocale);

	if (baselocale_t == NULL)
	{
		newloc = MemoryContextAlloc(TopMemoryContext,
									sizeof(test_locale_t));
		strncpy(newloc->lc_collate, "C", LOCALE_NAME_LEN);
		strncpy(newloc->lc_ctype, "C", LOCALE_NAME_LEN);
		newloc->reverse_sort = false;
		newloc->reverse_case = false;
	}
	else
		newloc = (test_locale_t *) baselocale_t;

	if ((category & LC_COLLATE_MASK) != 0)
	{
		newloc->reverse_sort = locale_is_reverse(locale);
		strncpy(newloc->lc_collate, locale, LOCALE_NAME_LEN);
	}
	if ((category & LC_CTYPE_MASK) != 0)
	{
		newloc->reverse_case = locale_is_reverse(locale);
		strncpy(newloc->lc_ctype, locale, LOCALE_NAME_LEN);
	}

	return (locale_t) newloc;
}

static void
test_freelocale(locale_t loc)
{
	Assert(loc != LC_GLOBAL_LOCALE);
	Assert((test_locale_t *)loc != &current_setlocale);
	pfree(loc);
}

static locale_t
test_uselocale(locale_t loc)
{
	test_locale_t *result = current_uselocale;

	if (loc != NULL)
	{
		if (loc == LC_GLOBAL_LOCALE)
			current_uselocale = &current_setlocale;
		else
			current_uselocale = (test_locale_t *) loc;
	}

	if (result == &current_setlocale)
		return LC_GLOBAL_LOCALE;
	else
		return (locale_t) result;
}

#ifdef LC_VERSION_MASK
static const char *
test_querylocale(int mask, locale_t locale)
{
	test_locale_t *testlocale = (test_locale_t *)locale;
	Assert((mask & LC_VERSION_MASK) != 0);
	if (testlocale->reverse_sort)
		return "6.28";
	else
		return "3.14";
}
#endif			/* LC_VERSION_MASK */

#else			/* WIN32 */
static locale_t
_test_create_locale(int category, const char *locale)
{
	test_locale_t *newloc;

	newloc = MemoryContextAlloc(TopMemoryContext,
								sizeof(test_locale_t));
	strncpy(newloc->lc_collate, "C", LOCALE_NAME_LEN);
	strncpy(newloc->lc_ctype, "C", LOCALE_NAME_LEN);
	newloc->reverse_sort = false;
	newloc->reverse_case = false;

	if (category == LC_ALL || category == LC_COLLATE)
	{
		if (locale_is_reverse(locale))
			newloc->reverse_sort = true;
		else
			newloc->reverse_sort = false;
		strncpy(newloc->lc_collate, locale, LOCALE_NAME_LEN);
	}
	if (category == LC_ALL || category == LC_CTYPE)
	{
		if (locale_is_reverse(locale))
			newloc->reverse_case = true;
		else
			newloc->reverse_case = false;
		strncpy(newloc->lc_ctype, locale, LOCALE_NAME_LEN);
	}

	return (locale_t) newloc;
}
#endif			/* WIN32 */

#endif			/* HAVE_LOCALE_T */

static size_t
test_wcstombs(char *dest, const wchar_t *src, size_t n)
{
	return wcstombs(dest, src, n);
}

static size_t
test_mbstowcs(wchar_t *dest, const char *src, size_t n)
{
	return mbstowcs(dest, src, n);
}

#ifdef HAVE_LOCALE_T
#ifdef HAVE_WCSTOMBS_L
static size_t
test_wcstombs_l(char *dest, const wchar_t *src, size_t n, locale_t loc)
{
	return wcstombs(dest, src, n);
}
#endif
#ifdef HAVE_MBSTOWCS_L
static size_t
test_mbstowcs_l(wchar_t *dest, const char *src, size_t n, locale_t loc)
{
	return mbstowcs(dest, src, n);
}
#endif
#endif

static int
test_strcoll_internal(const char *s1, const char *s2, bool reverse)
{
	int ret = strcmp(s1, s2);
	return reverse ? -ret : ret;
}

static int
test_strcoll(const char *s1, const char *s2)
{
	bool reverse = current_locale()->reverse_sort;
	return test_strcoll_internal(s1, s2, reverse);
}

static int
test_wcscoll_internal(const wchar_t *ws1, const wchar_t *ws2, bool reverse)
{
	int ret = wcscmp(ws1, ws2);
	return reverse ? -ret : ret;
}
static int
test_wcscoll(const wchar_t *ws1, const wchar_t *ws2)
{
	bool reverse = current_locale()->reverse_sort;
	return test_wcscoll_internal(ws1, ws2, reverse);
}

static size_t
test_strxfrm_internal(char *s1, const char *s2, size_t n, bool reverse)
{
	size_t			 result_size = strlen(s2) + 1;

	if (n > result_size)
	{
		strncpy(s1, s2, n);
		s1[result_size] = '\0';

		if (reverse)
		{
			unsigned char *dest = (unsigned char *)s1;
			for (int i = 0; i < result_size; i++)
				dest[i] ^= (unsigned char) 0xFF;
		}
	}

	return result_size;
}

static size_t
test_strxfrm(char *s1, const char * s2, size_t n)
{
	bool reverse = current_locale()->reverse_sort;
	return test_strxfrm_internal(s1, s2, n, reverse);
}

#ifdef HAVE_LOCALE_T
static int
test_strcoll_l(const char *s1, const char *s2, locale_t loc)
{
	test_locale_t *testlocale = (test_locale_t *)loc;
	bool reverse = testlocale->reverse_sort;
	return test_strcoll_internal(s1, s2, reverse);
}

static int
test_wcscoll_l(const wchar_t *ws1, const wchar_t *ws2, locale_t locale)
{
	test_locale_t *testlocale = (test_locale_t *) locale;
	bool reverse = testlocale->reverse_sort;
	return test_wcscoll_internal(ws1, ws2, reverse);
}

static size_t
test_strxfrm_l(char *s1, const char * s2, size_t n, locale_t loc)
{
	test_locale_t *testlocale = (test_locale_t *) loc;
	bool reverse = testlocale->reverse_sort;
	return test_strxfrm_internal(s1, s2, n, reverse);
}
#endif			 /* HAVE_LOCALE_T */

static int
test_case_internal(int c, bool toupper)
{
	if (toupper && ('a' <= c && c <= 'z'))
		return c - ('a' - 'A');
	else if (!toupper && ('A' <= c && c <= 'Z'))
		return c + ('a' - 'A');
	else
		return c;
}

static int
test_tolower(int c)
{
	bool reverse = current_locale()->reverse_case;
	return test_case_internal(c, reverse ? true : false);
}

static int
test_toupper(int c)
{
	bool reverse = current_locale()->reverse_case;
	return test_case_internal(c, reverse ? false : true);
}

static int
test_iswalnum_internal(wint_t wc)
{
	if (('A' <= wc && wc <= 'Z') ||
		('a' <= wc && wc <= 'z') ||
		('0' <= wc && wc <= '9'))
		return 1;
	return 0;
}

static int
test_iswalnum(wint_t wc)
{
	return test_iswalnum_internal(wc);
}

static wint_t
test_wcase_internal(wint_t wc, bool toupper)
{
	if (toupper && ('a' <= wc && wc <= 'z'))
		return wc - ('a' - 'A');
	else if (!toupper && ('A' <= wc && wc <= 'Z'))
		return wc + ('a' - 'A');
	else
		return wc;
}

static wint_t
test_towlower(wint_t wc)
{
	bool reverse = current_locale()->reverse_case;
	return test_wcase_internal(wc, reverse ? true : false);
}

static wint_t
test_towupper(wint_t wc)
{
	bool reverse = current_locale()->reverse_case;
	return test_wcase_internal(wc, reverse ? false : true);
}

#ifdef HAVE_LOCALE_T
static int
test_tolower_l(int c, locale_t locale)
{
	test_locale_t *testlocale = (test_locale_t *) locale;
	bool reverse = testlocale->reverse_case;
	return test_case_internal(c, reverse ? true : false);
}

static int
test_toupper_l(int c, locale_t locale)
{
	test_locale_t *testlocale = (test_locale_t *) locale;
	bool reverse = testlocale->reverse_case;
	return test_case_internal(c, reverse ? false : true);
}

static int
test_iswalnum_l(wint_t wc, locale_t locale)
{
	return test_iswalnum_internal(wc);
}

static wint_t
test_towlower_l(wint_t wc, locale_t locale)
{
	test_locale_t *testlocale = (test_locale_t *) locale;
	bool reverse = testlocale->reverse_case;
	return test_wcase_internal(wc, reverse ? true : false);
}

static wint_t
test_towupper_l(wint_t wc, locale_t locale)
{
	test_locale_t *testlocale = (test_locale_t *) locale;
	bool reverse = testlocale->reverse_case;
	return test_wcase_internal(wc, reverse ? false : true);
}
#endif			 /* HAVE_LOCALE_T */

pg_libc_library *
test_get_libc_library(const char *collate, const char *ctype,
					  const char *version)
{
	pg_libc_library *lib = NULL;

	if (test_libc_library != NULL)
		return test_libc_library;

	ereport(LOG, (errmsg("loading custom libc provider for test_collation_lib_hooks")));

	lib = MemoryContextAllocZero(TopMemoryContext, sizeof(pg_libc_library));
#if defined(__GLIBC__)
	lib->libc_version = test_libc_version;
#elif defined(WIN32)
	lib->GetNLSVersionEx = test_GetNLSVersionEx;
#endif
	lib->c_setlocale = test_setlocale;
#ifdef HAVE_LOCALE_T
#ifndef WIN32
	lib->c_newlocale = test_newlocale;
	lib->c_freelocale = test_freelocale;
	lib->c_uselocale = test_uselocale;
#ifdef LC_VERSION_MASK
	lib->c_querylocale = test_querylocale;
#endif
#else
	lib->_create_locale = _test_create_locale;
#endif
#endif
	lib->c_wcstombs = test_wcstombs;
	lib->c_mbstowcs = test_mbstowcs;
#ifdef HAVE_LOCALE_T
#ifdef HAVE_WCSTOMBS_L
	lib->c_wcstombs_l = test_wcstombs_l;
#endif
#ifdef HAVE_MBSTOWCS_L
	lib->c_mbstowcs_l = test_mbstowcs_l;
#endif
#endif
	lib->c_strcoll = test_strcoll;
	lib->c_wcscoll = test_wcscoll;
	lib->c_strxfrm = test_strxfrm;
#ifdef HAVE_LOCALE_T
	lib->c_strcoll_l = test_strcoll_l;
	lib->c_wcscoll_l = test_wcscoll_l;
	lib->c_strxfrm_l = test_strxfrm_l;
#endif
	lib->c_tolower = test_tolower;
	lib->c_toupper = test_toupper;
	lib->c_iswalnum = test_iswalnum;
	lib->c_towlower = test_towlower;
	lib->c_towupper = test_towupper;
#ifdef HAVE_LOCALE_T
	lib->c_tolower_l = test_tolower_l;
	lib->c_toupper_l = test_toupper_l;
	lib->c_iswalnum_l = test_iswalnum_l;
	lib->c_towlower_l = test_towlower_l;
	lib->c_towupper_l = test_towupper_l;
#endif

	test_libc_library = lib;
	return lib;
}
