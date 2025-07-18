/*-----------------------------------------------------------------------
 *
 * PostgreSQL NLS utilities
 *
 * Portions Copyright (c) 2002-2025, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/pg_nls.c
 *
 * Platform-independent wrappers for message translation functions. The
 * LC_CTYPE and LC_MESSAGES settings are set with pg_nls_set_locale() and the
 * state is managed internally to this file, regardless of the outside
 * settings from setlocale() or uselocale().
 *
 * The implementation prefers the "_l()" variants of functions, then
 * secondarily a temporary uselocale() setting (thread safe), and lastly a
 * temporary setlocale() setting (which can be made thread safe on windows).
 *
 * This mechanism improves thread safety (on most platforms), and provides
 * better separation between the behavior of NLS and other behaviors like
 * isupper(), etc.
 *
 *-----------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/pg_nls.h"

/*
 * Represents global LC_CTYPE and LC_MESSAGES settings, for the purpose of
 * message translation. LC_CTYPE in the postmaster comes from the environment,
 * and in a backend comes from pg_database.datctype. LC_MESSAGES comes from a
 * GUC, and must be kept up to date.
 *
 * If there's no uselocale(), keep the string values instead, and use
 * setlocale().
 */
#ifdef HAVE_USELOCALE

static locale_t nls_locale = (locale_t) 0;

#else

static char *nls_lc_ctype = NULL;
static char *nls_lc_messages = NULL;

typedef struct SaveLocale
{
#ifndef WIN32
	char	   *lc_ctype;
	char	   *lc_messages;
#else
	int			config_thread_locale;
	wchar_t    *lc_ctype;
	wchar_t    *lc_messages;
#endif							/* WIN32 */
}			SaveLocale;

#endif							/* !HAVE_USELOCALE */

/*
 * In this file, dgettext and dngettext have their original meanings.
 */

#undef dgettext
#undef dngettext

/*
 * Set the LC_CTYPE and LC_MESSAGES to be used for message translation.
 */
void
pg_nls_set_locale(const char *ctype, const char *messages)
{
	if (ctype)
	{
#ifdef HAVE_USELOCALE
		locale_t	loc = 0;

		errno = 0;
		loc = newlocale(LC_CTYPE_MASK, ctype, nls_locale);
		if (!loc)
			report_newlocale_failure(ctype);
		nls_locale = loc;
#else
		if (!check_locale(LC_CTYPE, ctype, NULL))
			report_newlocale_failure(ctype);
		if (nls_lc_ctype)
			pfree(nls_lc_ctype);
		nls_lc_ctype = MemoryContextStrdup(TopMemoryContext, ctype);
#endif

		/*
		 * Use the right encoding in translated messages.  Under ENABLE_NLS,
		 * let pg_bind_textdomain_codeset() figure it out.  Under !ENABLE_NLS,
		 * message format strings are ASCII, but database-encoding strings may
		 * enter the message via %s.  This makes the overall message encoding
		 * equal to the database encoding.
		 */
#ifdef ENABLE_NLS
		SetMessageEncoding(pg_bind_textdomain_codeset(textdomain(NULL)));
#else
		SetMessageEncoding(GetDatabaseEncoding());
#endif
	}

	if (messages)
	{
#ifdef HAVE_USELOCALE
		locale_t	loc = 0;

		errno = 0;
		loc = newlocale(LC_MESSAGES_MASK, messages, nls_locale);
		if (!loc)
			report_newlocale_failure(messages);
		nls_locale = loc;
#else
#ifdef LC_MESSAGES
		if (!check_locale(LC_MESSAGES, messages, NULL))
			report_newlocale_failure(messages);
#endif
		if (nls_lc_messages)
			pfree(nls_lc_messages);
		nls_lc_messages = MemoryContextStrdup(TopMemoryContext, messages);
#endif
	}
}

#ifdef ENABLE_NLS

#ifdef HAVE_USELOCALE

#ifndef HAVE_DGETTEXT_L
static char *
dgettext_l(const char *domainname, const char *msgid, locale_t loc)
{
	char	   *result;
	locale_t	save_loc = uselocale(loc);

	result = dgettext(domainname, msgid);
	uselocale(save_loc);
	return result;
}
#endif							/* HAVE_DGETTEXT_L */

#ifndef HAVE_DNGETTEXT_L
static char *
dngettext_l(const char *domainname, const char *s, const char *p,
			unsigned long int n, locale_t loc)
{
	char	   *result;
	locale_t	save_loc = uselocale(loc);

	result = dngettext(domainname, s, p, n);
	uselocale(save_loc);
	return result;
}
#endif							/* HAVE_DNGETTEXT_L */

static char *
pg_strerror_l(int errnum, locale_t loc)
{
	char	   *result;
	locale_t	save_loc = uselocale(loc);

	result = pg_strerror(errnum);
	uselocale(save_loc);
	return result;
}

static char *
pg_strerror_r_l(int errnum, char *buf, size_t buflen, locale_t loc)
{
	char	   *result;
	locale_t	save_loc = uselocale(loc);

	result = pg_strerror_r(errnum, buf, buflen);
	uselocale(save_loc);
	return result;
}

#else							/* !HAVE_USELOCALE */

static bool
save_message_locale(SaveLocale * save)
{
#ifndef WIN32
	char	   *tmp;

	/*
	 * This path -- ENABLE_NLS, !HAVE_USELOCALE, !WIN32 -- is not thread safe,
	 * but is only known to be used on NetBSD.
	 */
	tmp = setlocale(LC_CTYPE, NULL);
	if (!tmp)
		return false;
	save->lc_ctype = pstrdup(tmp);

	tmp = setlocale(LC_MESSAGES, NULL);
	if (!tmp)
		return false;
	save->lc_messages = pstrdup(tmp);

	return true;
#else
	wchar_t    *tmp;

	/* Put setlocale() into thread-local mode. */
	save->config_thread_locale = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);

	/*
	 * Capture the current values as wide strings.  Otherwise, we might not be
	 * able to restore them if their names contain non-ASCII characters and
	 * the intermediate locale changes the expected encoding.  We don't want
	 * to leave the caller in an unexpected state by failing to restore, or
	 * crash the runtime library.
	 */
	tmp = _wsetlocale(LC_CTYPE, NULL);
	if (!tmp || !(tmp = wcsdup(tmp)))
		return false;
	*save->lc_ctype = tmp;

	tmp = _wsetlocale(LC_MESSAGES, NULL);
	if (!tmp || !(tmp = wcsdup(tmp)))
		return false;
	*save->lc_messages = tmp;

	return true;
#endif
}

static void
restore_message_locale(SaveLocale * save)
{
#ifndef WIN32
	if (save->lc_ctype)
	{
		setlocale(LC_CTYPE, save->lc_ctype);
		pfree(save->lc_ctype);
		save->lc_ctype = NULL;
	}
	if (save->lc_messages)
	{
		setlocale(LC_MESSAGES, save->lc_messages);
		pfree(save->lc_messages);
		save->lc_messages = NULL;
	}
#else
	if (save->lc_ctype)
	{
		_wsetlocale(LC_CTYPE, save->lc_ctype);
		free(save->lc_ctype);
		save->lc_ctype = NULL;
	}
	if (save->lc_messages)
	{
		_wsetlocale(LC_MESSAGES, save->lc_messages);
		free(save->lc_messages);
		save->lc_messages = NULL;
	}
	_configthreadlocale(save->config_thread_locale);
#endif
}

static char *
dgettext_l(const char *domainname, const char *msgid, const char *lc_ctype,
		   const char *lc_messages)
{
	SaveLocale	save;

	if (save_message_locale(&save))
	{
		char	   *result;

		(void) setlocale(LC_CTYPE, lc_ctype);
		(void) setlocale(LC_MESSAGES, lc_messages);

		result = dgettext(domainname, msgid);
		restore_message_locale(&save);
		return result;
	}
	else
		return dgettext(domainname, msgid);
}

static char *
dngettext_l(const char *domainname, const char *s, const char *p,
			unsigned long int n, const char *lc_ctype,
			const char *lc_messages)
{
	SaveLocale	save;

	if (save_message_locale(&save))
	{
		char	   *result;

		(void) setlocale(LC_CTYPE, lc_ctype);
		(void) setlocale(LC_MESSAGES, lc_messages);

		result = dngettext(domainname, s, p, n);
		restore_message_locale(&save);
		return result;
	}
	else
		return dngettext(domainname, s, p, n);
}

static char *
pg_strerror_l(int errnum, const char *lc_ctype, const char *lc_messages)
{
	SaveLocale	save;

	if (save_message_locale(&save))
	{
		char	   *result;

		(void) setlocale(LC_CTYPE, lc_ctype);
		(void) setlocale(LC_MESSAGES, lc_messages);

		result = pg_strerror(errnum);
		restore_message_locale(&save);
		return result;
	}
	else
		return pg_strerror(errnum);
}

static char *
pg_strerror_r_l(int errnum, char *buf, size_t buflen, const char *lc_ctype,
				const char *lc_messages)
{
	SaveLocale	save;

	if (save_message_locale(&save))
	{
		char	   *result;

		(void) setlocale(LC_CTYPE, lc_ctype);
		(void) setlocale(LC_MESSAGES, lc_messages);

		result = pg_strerror_r(errnum, buf, buflen);
		restore_message_locale(&save);
		return result;
	}
	else
		return pg_strerror_r(errnum, buf, buflen);
}

#endif							/* !HAVE_USELOCALE */

/*
 * dgettext() with nls_locale, if set.
 */
char *
pg_nls_dgettext(const char *domainname, const char *msgid)
{
#ifdef HAVE_USELOCALE
	if (nls_locale)
		return dgettext_l(domainname, msgid, nls_locale);
#else
	if (nls_lc_ctype)
		return dgettext_l(domainname, msgid, nls_lc_ctype,
						  nls_lc_messages);
#endif
	else
		return dgettext(domainname, msgid);
}

/*
 * dngettext() with nls_locale, if set.
 */
char *
pg_nls_dngettext(const char *domainname, const char *s, const char *p,
				 unsigned long int n)
{
#ifdef HAVE_USELOCALE
	if (nls_locale)
		return dngettext_l(domainname, s, p, n, nls_locale);
#else
	if (nls_lc_ctype)
		return dngettext_l(domainname, s, p, n, nls_lc_ctype,
						   nls_lc_messages);
#endif
	else
		return dngettext(domainname, s, p, n);
}

/*
 * pg_strerror() with nls_locale, if set.
 */
char *
pg_nls_strerror(int errnum)
{
#ifdef HAVE_USELOCALE
	if (nls_locale)
		return pg_strerror_l(errnum, nls_locale);
#else
	if (nls_lc_ctype)
		return pg_strerror_l(errnum, nls_lc_ctype, nls_lc_messages);
#endif
	else
		return pg_strerror(errnum);
}

/*
 * pg_strerror_r() with nls_locale, if set.
 */
char *
pg_nls_strerror_r(int errnum, char *buf, size_t buflen)
{
#ifdef HAVE_USELOCALE
	if (nls_locale)
		return pg_strerror_r_l(errnum, buf, buflen, nls_locale);
#else
	if (nls_lc_ctype)
		return pg_strerror_r_l(errnum, buf, buflen, nls_lc_ctype,
							   nls_lc_messages);
#endif
	else
		return pg_strerror_r(errnum, buf, buflen);
}

#endif							/* ENABLE_NLS */
