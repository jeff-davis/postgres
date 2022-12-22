/*--------------------------------------------------------------------------
 *
 * test_collation_lib_hooks.c
 *		Code for testing collation provider library hooks
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/test_collation_lib_hooks/test_collation_lib_hooks.c
 *
 * Test implementations of libc-like and icu-like collation providers.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"

#include "test_collation_lib_hooks.h"

static get_libc_library_hook_type prev_get_libc_library_hook = NULL;
#ifdef USE_ICU
static get_icu_library_hook_type prev_get_icu_library_hook = NULL;
#endif

PG_MODULE_MAGIC;

/*
 * Module load callback
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR, (errmsg("test_collation_lib_hooks must be loaded via shared_preload_libraries")));

	prev_get_libc_library_hook = get_libc_library_hook;
	get_libc_library_hook = test_get_libc_library;

#ifdef USE_ICU
	prev_get_icu_library_hook = get_icu_library_hook;
	get_icu_library_hook = test_get_icu_library;
#endif

	init_libc_hook();
}
