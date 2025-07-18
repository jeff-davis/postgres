/*-----------------------------------------------------------------------
 *
 * PostgreSQL NLS utilities
 *
 * src/include/utils/pg_nls.h
 *
 * Copyright (c) 2002-2025, PostgreSQL Global Development Group
 *
 *-----------------------------------------------------------------------
 */

#ifndef _PG_NLS_
#define _PG_NLS_

extern void pg_nls_set_locale(const char *ctype, const char *messages);

#ifdef ENABLE_NLS

extern char *pg_nls_dgettext(const char *domainname, const char *msgid)
			pg_attribute_format_arg(2);
extern char *pg_nls_dngettext(const char *domainname, const char *s,
							  const char *p, unsigned long int n)
			pg_attribute_format_arg(2) pg_attribute_format_arg(3);
extern char *pg_nls_strerror(int errnum);
extern char *pg_nls_strerror_r(int errnum, char *buf, size_t buflen);

#endif

#endif							/* _PG_NLS_ */
