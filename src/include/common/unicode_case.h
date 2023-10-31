/*-------------------------------------------------------------------------
 *
 * unicode_case.h
 *	  Routines for converting character case.
 *
 * These definitions can be used by both frontend and backend code.
 *
 * Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * src/include/common/unicode_case.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNICODE_CASE_H
#define UNICODE_CASE_H

#include "mb/pg_wchar.h"

/*
 * The maximum number of code points that can result from case mapping. See
 * Unicode section 5.18, "Case Mapping". Expansion can only happen when using
 * the special casing.
 *
 * NB: This is the maximum expansion of code points, which is not the same as
 * the maximum expansion of encoded length.
 */
#define PG_U_MAX_CASE_EXPANSION 3

pg_wchar unicode_lowercase(pg_wchar ucs, const pg_wchar **special);
pg_wchar unicode_titlecase(pg_wchar ucs, const pg_wchar **special);
pg_wchar unicode_uppercase(pg_wchar ucs, const pg_wchar **special);

#endif							/* UNICODE_CASE_H */
