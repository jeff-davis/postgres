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
	union
	{
#ifdef HAVE_LOCALE_T
		locale_t	lt;
#endif
#ifdef USE_ICU
		struct
		{
			char		*locale;
			UCollator	*ucol;
		}			icu;
#endif
		int			dummy;		/* in case we have neither LOCALE_T nor ICU */
	}			info;
};

typedef struct pg_locale_struct *(*pg_newlocale_hook_type)(
	char provider, bool deterministic, const char *collate, const char *ctype,
	const char *version);

typedef char *(*pg_setlocale_hook_type)(int category, const char *locale);

extern PGDLLIMPORT pg_newlocale_hook_type pg_newlocale_hook;
extern PGDLLIMPORT pg_setlocale_hook_type pg_setlocale_hook;

#endif							/* _PG_LOCALE_INTERNAL_ */
