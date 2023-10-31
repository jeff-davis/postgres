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

typedef enum
{
	CaseLower = 0,
	CaseTitle = 1,
	CaseUpper = 2,
	NCaseKind
}			CaseKind;

pg_wchar	unicode_case_simple(pg_wchar ucs, CaseKind casekind);
size_t		unicode_convert_case(char *dst, size_t dstsize, const char *src,
								 size_t srclen, CaseKind casekind, bool full);

#endif							/* UNICODE_CASE_H */
