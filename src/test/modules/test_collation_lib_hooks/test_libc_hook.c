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

#define TEST_LIBC_VERSION "3.14159"
#define LOCALE_NAME_LEN 64

typedef struct test_locale_t
{
	bool reverse_sort;
	bool reverse_case;
	char lc_collate[LOCALE_NAME_LEN];
	char lc_ctype[LOCALE_NAME_LEN];
} test_locale_t;

static pg_libc_library *test_libc_library = NULL;
static test_locale_t current_setlocale = { .lc_collate = "C", .lc_ctype = "C" };
static test_locale_t *current_uselocale = (test_locale_t *)LC_GLOBAL_LOCALE;

#ifdef HAVE_LOCALE_T
static locale_t c_locale_t = NULL;
#endif

void
init_libc_hook()
{
	c_locale_t = newlocale(LC_ALL_MASK, "C", NULL);
}

static bool
locale_is_reverse(const char *locale)
{
	if (strcmp(locale, "DESC") == 0)
		return true;
	else
		return false;
}

static const char *
test_libc_version()
{
	return TEST_LIBC_VERSION;
}

static char *
test_setlocale(int category, const char *locale)
{
	if (category == LC_ALL)
	{
		if (locale)
		{
			if (locale_is_reverse(locale))
			{
				current_setlocale.reverse_sort = true;
				current_setlocale.reverse_case = true;
			}
			else
			{
				current_setlocale.reverse_sort = false;
				current_setlocale.reverse_case = false;
			}
			strncpy(current_setlocale.lc_collate, locale, LOCALE_NAME_LEN);
			strncpy(current_setlocale.lc_ctype, locale, LOCALE_NAME_LEN);
		}
		return current_setlocale.lc_collate;
	}
	else if (category == LC_COLLATE)
	{
		if (locale)
			strncpy(current_setlocale.lc_collate, locale, LOCALE_NAME_LEN);

		return current_setlocale.lc_collate;
	}
	else if (category == LC_CTYPE)
	{
		if (locale)
			strncpy(current_setlocale.lc_ctype, locale, LOCALE_NAME_LEN);

		return current_setlocale.lc_ctype;
	}
	else
		Assert(false);
}

#ifdef HAVE_LOCALE_T

static locale_t
test_newlocale(int category, const char *locale, locale_t baselocale_t)
{
	test_locale_t *newloc;

	if (baselocale_t == NULL)
	{
		newloc = MemoryContextAlloc(TopMemoryContext,
									sizeof(test_locale_t));
		strncpy(newloc->lc_collate, "C", LOCALE_NAME_LEN);
		strncpy(newloc->lc_ctype, "C", LOCALE_NAME_LEN);
	}
	else
		newloc = (test_locale_t *) baselocale_t;

	if (category == LC_ALL_MASK || category == LC_COLLATE_MASK)
	{
		if (locale_is_reverse(locale))
			newloc->reverse_sort = true;
		else
			newloc->reverse_sort = false;
		strncpy(newloc->lc_collate, locale, LOCALE_NAME_LEN);
	}
	if (category == LC_ALL_MASK || category == LC_CTYPE_MASK)
	{
		if (locale_is_reverse(locale))
			newloc->reverse_case = true;
		else
			newloc->reverse_case = false;
		strncpy(newloc->lc_ctype, locale, LOCALE_NAME_LEN);
	}

	return (locale_t) newloc;
}

#ifndef WIN32
static void
test_freelocale(locale_t loc)
{
	pfree(loc);
}
#endif

#ifdef WIN32
static locale_t
_test_create_locale(int category, const char *locale)
{
	return test_newlocale(category, locale, NULL);
}
#endif

static locale_t
test_uselocale(locale_t loc)
{
	test_locale_t *result = current_uselocale;

	if (loc != NULL)
		current_uselocale = (test_locale_t *) loc;

	return (locale_t) result;
}
#endif			/* HAVE_LOCALE_T */

static int
test_strcoll(const char *s1, const char *s2)
{
	char			*save		= pstrdup(setlocale(LC_COLLATE, NULL));
	int				 ret;

	setlocale(LC_COLLATE, "C");
	ret = strcoll(s1, s2);
	setlocale(LC_COLLATE, save);
	pfree(save);

	if (current_setlocale.reverse_sort)
		return -ret;
	else
		return ret;
}

static int
test_wcscoll(const wchar_t *ws1, const wchar_t *ws2)
{
	char			*save		= pstrdup(setlocale(LC_COLLATE, NULL));
	int				 ret;

	setlocale(LC_COLLATE, "C");
	ret = wcscoll(ws1, ws2);
	setlocale(LC_COLLATE, save);
	pfree(save);

	if (current_setlocale.reverse_sort)
		return -ret;
	else
		return ret;
}

static size_t
test_strxfrm(char *s1, const char * s2, size_t n)
{
	char			*save		 = pstrdup(setlocale(LC_COLLATE, NULL));
	int				 ret;
	size_t			 result_size;

	setlocale(LC_COLLATE, "C");
	ret = strxfrm(s1, s2, n);
	setlocale(LC_COLLATE, save);
	pfree(save);

	result_size = ret + 1;

	if (n >= result_size)
	{
		s1[ret] = '\0';

		if (current_setlocale.reverse_sort)
			for (int i = 0; i < result_size; i++)
				*((unsigned char *) s1 + i) ^= (char) 0xff;
	}

	return result_size;
}

#ifdef HAVE_LOCALE_T
static int
test_strcoll_l(const char *s1, const char *s2, locale_t loc)
{
	test_locale_t *testlocale = (test_locale_t *)loc;
	int ret = strcoll_l(s1, s2, c_locale_t);

	if (testlocale->reverse_sort)
		return -ret;
	else
		return ret;
}

static int
test_wcscoll_l(const wchar_t *ws1, const wchar_t *ws2, locale_t locale)
{
	test_locale_t *testlocale = (test_locale_t *) locale;
	int ret = wcscoll_l(ws1, ws2, c_locale_t);

	if (testlocale->reverse_sort)
		return -ret;
	else
		return ret;
}

static size_t
test_strxfrm_l(char *s1, const char * s2, size_t n, locale_t loc)
{
	test_locale_t *testlocale = (test_locale_t *)loc;
	size_t ret = strxfrm_l(s1, s2, n, c_locale_t);
	size_t result_size = ret + 1;

	if (n >= result_size)
	{
		s1[ret] = '\0';

		if (testlocale->reverse_sort)
			for (int i = 0; i < result_size; i++)
				*((unsigned char *) s1 + i) ^= (unsigned char) 0xff;
	}

	return result_size;
}
#endif			 /* HAVE_LOCALE_T */

static int
test_iswalnum(wint_t wc)
{
	char			*save		= pstrdup(setlocale(LC_COLLATE, NULL));
	int				 ret;

	setlocale(LC_COLLATE, "C");
	ret = iswalnum(wc);
	setlocale(LC_COLLATE, save);
	pfree(save);

	return ret;
}

static wint_t
test_towlower(wint_t wc)
{
	char			*save		= pstrdup(setlocale(LC_COLLATE, NULL));
	wint_t			 ret;

	setlocale(LC_COLLATE, "C");
	if (current_setlocale.reverse_case)
		ret = towupper(wc);
	else
		ret = towlower(wc);
	setlocale(LC_COLLATE, save);
	pfree(save);

	return ret;
}

static wint_t
test_towupper(wint_t wc)
{
	char			*save		= pstrdup(setlocale(LC_COLLATE, NULL));
	wint_t			 ret;

	setlocale(LC_COLLATE, "C");
	if (current_setlocale.reverse_case)
		ret = towlower(wc);
	else
		ret = towupper(wc);
	setlocale(LC_COLLATE, save);
	pfree(save);

	return ret;
}

#ifdef HAVE_LOCALE_T
static int
test_iswalnum_l(wint_t wc, locale_t locale)
{
	return iswalnum_l(wc, c_locale_t);
}

static wint_t
test_towlower_l(wint_t wc, locale_t locale)
{
	test_locale_t *testlocale = (test_locale_t *) locale;

	if (testlocale->reverse_case)
		return towupper_l(wc, c_locale_t);
	else
		return towlower_l(wc, c_locale_t);
}

static wint_t
test_towupper_l(wint_t wc, locale_t locale)
{
	test_locale_t *testlocale = (test_locale_t *) locale;

	if (testlocale->reverse_case)
		return towlower_l(wc, c_locale_t);
	else
		return towupper_l(wc, c_locale_t);
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

	lib = MemoryContextAlloc(TopMemoryContext, sizeof(pg_libc_library));
	lib->libc_version = test_libc_version;
	lib->c_setlocale = test_setlocale;
#ifdef HAVE_LOCALE_T
#ifndef WIN32
	lib->c_newlocale = test_newlocale;
	lib->c_freelocale = test_freelocale;
	lib->c_uselocale = test_uselocale;
#else
	lib->_create_locale = _test_create_locale;
#endif
#endif
	lib->c_wcstombs = wcstombs;
	lib->c_mbstowcs = mbstowcs;
#ifdef HAVE_LOCALE_T
#ifdef HAVE_WCSTOMBS_L
	lib->c_wcstombs_l = wcstombs_l;
#endif
#ifdef HAVE_MBSTOWCS_L
	lib->c_mbstowcs_l = mbstowcs_l;
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
	lib->c_iswalnum = test_iswalnum;
	lib->c_towlower = test_towlower;
	lib->c_towupper = test_towupper;
#ifdef HAVE_LOCALE_T
	lib->c_iswalnum_l = test_iswalnum_l;
	lib->c_towlower_l = test_towlower_l;
	lib->c_towupper_l = test_towupper_l;
#endif

	test_libc_library = lib;
	return lib;
}
