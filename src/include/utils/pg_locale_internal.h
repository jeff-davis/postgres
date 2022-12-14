/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * src/include/utils/pg_locale_internal.h
 *
 * Copyright (c) 2002-2022, PostgreSQL Global Development Group
 *
 *-----------------------------------------------------------------------
 */


#ifndef _PG_LOCALE_INTERNAL_
#define _PG_LOCALE_INTERNAL_

#include <wchar.h>
#include <wctype.h>

#ifdef USE_ICU
#include <unicode/ubrk.h>
#include <unicode/ucnv.h>
#include <unicode/ucol.h>
#endif

#ifdef USE_ICU
/*
 * ucol_strcollUTF8() was introduced in ICU 50, but it is buggy before ICU 53.
 * (see
 * <https://www.postgresql.org/message-id/flat/f1438ec6-22aa-4029-9a3b-26f79d330e72%40manitou-mail.org>)
 */
#if U_ICU_VERSION_MAJOR_NUM >= 53
#define HAVE_UCOL_STRCOLLUTF8 1
#else
#undef HAVE_UCOL_STRCOLLUTF8
#endif
#endif

typedef struct pg_libc_library
{
	/* version */
#if defined(__GLIBC__)
	const char *(*libc_version) (void);
#elif defined(WIN32)
	BOOL (*GetNLSVersionEx) (NLS_FUNCTION function, LPCWSTR lpLocaleName,
							 LPNLSVERSIONINFOEX lpVersionInformation);
#endif

	/* locale */
	char *(*c_setlocale) (int category, const char *locale);
#ifdef HAVE_LOCALE_T
#ifndef WIN32
	locale_t (*c_newlocale) (int category_mask, const char *locale,
						   locale_t base);
	void (*c_freelocale) (locale_t locobj);
	locale_t (*c_uselocale) (locale_t newloc);
#ifdef LC_VERSION_MASK
	const char *(*c_querylocale) (int mask, locale_t locale);
#endif
#else
	locale_t (*_create_locale) (int category, const char *locale);
#endif
#endif

	/* encoding */
	size_t (*c_wcstombs) (char *dest, const wchar_t *src, size_t n);
	size_t (*c_mbstowcs) (wchar_t *dest, const char *src, size_t n);
#ifdef HAVE_LOCALE_T
#ifdef HAVE_WCSTOMBS_L
	size_t (*c_wcstombs_l) (char *dest, const wchar_t *src, size_t n,
						  locale_t loc);
#endif
#ifdef HAVE_MBSTOWCS_L
	size_t (*c_mbstowcs_l) (wchar_t *dest, const char *src, size_t n,
						  locale_t loc);
#endif
#endif

	/* collation */
	int (*c_strcoll) (const char *s1, const char *s2);
	int (*c_wcscoll) (const wchar_t *ws1, const wchar_t *ws2);
	size_t (*c_strxfrm) (char *s1, const char * s2, size_t n);
#ifdef HAVE_LOCALE_T
	int (*c_strcoll_l) (const char *s1, const char *s2, locale_t locale);
	int (*c_wcscoll_l) (const wchar_t *ws1, const wchar_t *ws2,
					  locale_t locale);
	size_t (*c_strxfrm_l) (char *s1, const char * s2, size_t n,
						 locale_t locale);
#endif

	/* ctype */
	int (*c_tolower) (int c);
	int (*c_toupper) (int c);
	int (*c_iswalnum) (wint_t wc);
	wint_t (*c_towlower) (wint_t wc);
	wint_t (*c_towupper) (wint_t wc);
#ifdef HAVE_LOCALE_T
	int (*c_tolower_l) (int c, locale_t locale);
	int (*c_toupper_l) (int c, locale_t locale);
	int (*c_iswalnum_l) (wint_t wc, locale_t locale);
	wint_t (*c_towlower_l) (wint_t wc, locale_t locale);
	wint_t (*c_towupper_l) (wint_t wc, locale_t locale);
#endif
} pg_libc_library;

#define PG_LIBC_LIB(x) ((x)->info.libc.lib)

#ifdef USE_ICU
/*
 * An ICU library version that we're either linked against or have loaded at
 * runtime.
 */
typedef struct pg_icu_library
{
	int			major_version;
	int			minor_version;
	void		(*getICUVersion) (UVersionInfo info);
	void		(*getUnicodeVersion) (UVersionInfo into);
	void		(*getCLDRVersion) (UVersionInfo info, UErrorCode *status);
	UCollator  *(*openCollator) (const char *loc, UErrorCode *status);
	void		(*closeCollator) (UCollator *coll);
	void		(*getCollatorVersion) (const UCollator *coll, UVersionInfo info);
	void		(*getUCAVersion) (const UCollator *coll, UVersionInfo info);
	void		(*versionToString) (const UVersionInfo versionArray,
									char *versionString);
	UCollationResult (*strcoll) (const UCollator *coll,
								 const UChar *source,
								 int32_t sourceLength,
								 const UChar *target,
								 int32_t targetLength);
	UCollationResult (*strcollUTF8) (const UCollator *coll,
									 const char *source,
									 int32_t sourceLength,
									 const char *target,
									 int32_t targetLength,
									 UErrorCode *status);
	int32_t		(*getSortKey) (const UCollator *coll,
							   const UChar *source,
							   int32_t sourceLength,
							   uint8_t *result,
							   int32_t resultLength);
	int32_t		(*nextSortKeyPart) (const UCollator *coll,
									UCharIterator *iter,
									uint32_t state[2],
									uint8_t *dest,
									int32_t count,
									UErrorCode *status);
	void		(*setUTF8) (UCharIterator *iter,
							const char *s,
							int32_t length);
	const char *(*errorName) (UErrorCode code);
	int32_t		(*strToUpper) (UChar *dest,
							   int32_t destCapacity,
							   const UChar *src,
							   int32_t srcLength,
							   const char *locale,
							   UErrorCode *pErrorCode);
	int32_t		(*strToLower) (UChar *dest,
							   int32_t destCapacity,
							   const UChar *src,
							   int32_t srcLength,
							   const char *locale,
							   UErrorCode *pErrorCode);
	int32_t		(*strToTitle) (UChar *dest,
							   int32_t destCapacity,
							   const UChar *src,
							   int32_t srcLength,
							   UBreakIterator *titleIter,
							   const char *locale,
							   UErrorCode *pErrorCode);
	void		(*setAttribute) (UCollator *coll,
								 UColAttribute attr,
								 UColAttributeValue value,
								 UErrorCode *status);
	UConverter *(*openConverter) (const char *converterName,
								  UErrorCode *  	err);
	void		(*closeConverter) (UConverter *converter);
	int32_t		(*fromUChars) (UConverter *cnv,
							   char *dest,
							   int32_t destCapacity,
							   const UChar *src,
							   int32_t srcLength,
							   UErrorCode *pErrorCode);
	int32_t		(*toUChars) (UConverter *cnv,
							 UChar *dest,
							 int32_t destCapacity,
							 const char *src,
							 int32_t srcLength,
							 UErrorCode *pErrorCode);
	int32_t		(*toLanguageTag) (const char *localeID,
								  char *langtag,
								  int32_t langtagCapacity,
								  UBool strict,
								  UErrorCode *err);
	int32_t		(*getDisplayName) (const char *localeID,
								   const char *inLocaleID,
								   UChar *result,
								   int32_t maxResultSize,
								   UErrorCode *err);
	int32_t		(*countAvailable) (void);
	const char *(*getAvailable) (int32_t n);
} pg_icu_library;

#define PG_ICU_LIB(x) ((x)->info.icu.lib)

#endif

/*
 * We define our own wrapper around locale_t so we can keep the same
 * function signatures for all builds, while not having to create a
 * fake version of the standard type locale_t in the global namespace.
 * pg_locale_t is occasionally checked for truth, so make it a pointer.
 */
struct pg_locale_struct
{
	char		provider;
	bool		deterministic;
	char	   *collate;
	char	   *ctype;
	union
	{
		struct
		{
#ifdef HAVE_LOCALE_T
			locale_t	lt;
#endif
			pg_libc_library *lib;
		}			libc;
#ifdef USE_ICU
		struct
		{
			UCollator		*ucol;
			pg_icu_library	*lib;
		}			icu;
#endif
	}			info;
};

typedef pg_libc_library *(*get_libc_library_hook_type)(
	const char *collate, const char *ctype, const char *version);

extern PGDLLIMPORT get_libc_library_hook_type get_libc_library_hook;

extern pg_libc_library *get_default_libc_library(void);

#ifdef USE_ICU

typedef pg_icu_library *(*get_icu_library_hook_type)(
	const char *locale, const char *version);

extern PGDLLIMPORT get_icu_library_hook_type get_icu_library_hook;

extern pg_icu_library *get_default_icu_library(void);
extern int32_t icu_to_uchar(pg_icu_library *lib, UChar **buff_uchar,
							const char *buff, size_t nbytes);
extern int32_t icu_from_uchar(pg_icu_library *lib, char **result,
							  const UChar *buff_uchar, int32_t len_uchar);

#endif							/* USE_ICU */

#endif							/* _PG_LOCALE_INTERNAL_ */
