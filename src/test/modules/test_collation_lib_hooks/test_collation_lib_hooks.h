/*--------------------------------------------------------------------------
 *
 * test_collation_lib_hooks.h
 *		Definitions for collation library hooks.
 *
 * Copyright (c) 2015-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_collation_lib_hooks/test_collation_lib_hooks.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef TEST_COLLATION_LIB_HOOKS_H
#define TEST_COLLATION_LIB_HOOKS_H

#include "postgres.h"

#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/pg_locale_internal.h"

#ifdef USE_ICU
extern pg_icu_library *test_get_icu_library(const char *locale,
											const char *version);
#endif

#endif
