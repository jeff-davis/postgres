
#include "postgres.h"

#include <dlfcn.h>
#include <limits.h>

#include "common/string.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pg_locale_internal.h"

#ifndef USE_ICU
#error "ICU support is required to build icu_multilib"
#endif

/*
 * We don't want to call into dlopen'd ICU libraries that are newer than the
 * one we were compiled and linked against, just in case there is an
 * incompatible API change.
 */
#define PG_MAX_ICU_MAJOR U_ICU_VERSION_MAJOR_NUM

/*
 * The oldest ICU release we're likely to encounter, and that has all the
 * funcitons required.
 */
#define PG_MIN_ICU_MAJOR 50

/*
 * Enough to hold entries for minimum to maximum supported ICU versions, and
 * also the builtin ICU (if icu_multilib.include_builtin_icu is set).
 */
#define ICU_LIB_TABLESIZE (PG_MAX_ICU_MAJOR - PG_MIN_ICU_MAJOR + 2)

typedef struct icu_version
{
	int major;
	int minor;
} icu_version;

static const struct config_enum_entry log_level_options[] = {
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"debug", DEBUG2, true},
	{"log", LOG, false},
	{"info", INFO, true},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"error", ERROR, false},
	{NULL, 0, false}
};

PG_MODULE_MAGIC;

static get_icu_library_hook_type prev_icu_library_hook = NULL;

static pg_icu_library *icu_library_table[ICU_LIB_TABLESIZE] = {};

static char *icu_library_path = "";
static char *default_icu_version = "";
static icu_version default_version = { .major = -1, .minor = -1 };
static bool search_by_collator_version = false;
static bool include_builtin = true;
static int version_mismatch_log_level = WARNING;
static int library_search_log_level = DEBUG1;

static void initialize_gucs_1(void);
static void initialize_gucs_2(void);
static pg_icu_library *icu_multilib_hook(Oid collid, const char *locale,
										 const char *version);
static pg_icu_library *icu_multilib_search(Oid collid, const char *locale,
										   const char *version,
										   bool logOk);
static void load_all_libraries(void);
static pg_icu_library *load_icu_library(int major);
static pg_icu_library *get_icu_by_major_version(int major);
static void get_library_version(const pg_icu_library *lib, int *major,
								int *minor);

static bool check_string_version(char **newval, void **extra, GucSource source);
static void assign_default_version(const char *newval, void *extra);

void
_PG_init()
{
	/*TODO: error messages */
	if (!process_shared_preload_libraries_in_progress)
		elog(ERROR, "icu_multilib must be loaded by shared_preload_libraries");

	prev_icu_library_hook = get_icu_library_hook;
	get_icu_library_hook = icu_multilib_hook;

	initialize_gucs_1();
	load_all_libraries();
	initialize_gucs_2();
}

/* initialize GUCs before loading ICU libraries */
static void
initialize_gucs_1()
{
	/*
	 * The library search path is initialized once per server start and not
	 * changable -- this keeps the memory usage in TopMemoryContext bounded.
	 */
	DefineCustomStringVariable("icu_multilib.library_path",
							   "Filesystem path where ICU libraries are installed.",
							   NULL,
							   &icu_library_path,
							   "",
							   PGC_POSTMASTER,
							   0, NULL, NULL, NULL);
	DefineCustomEnumVariable("icu_multilib.version_mismatch_log_level",
							 "Level of log message when a collator version mismatch is detected.",
							 NULL,
							 &version_mismatch_log_level,
							 WARNING,
							 log_level_options,
							 PGC_SUSET,
							 0, NULL, NULL, NULL);
	DefineCustomEnumVariable("icu_multilib.library_search_log_level",
							 "Level of log messages related to searching for an ICU library.",
							 NULL,
							 &library_search_log_level,
							 DEBUG1,
							 log_level_options,
							 PGC_SUSET,
							 0, NULL, NULL, NULL);
}

/* initialize GUCs after loading ICU libraries */
static void
initialize_gucs_2()
{
	DefineCustomStringVariable("icu_multilib.default_icu_version",
							   "The version of the default ICU library.",
							   "Can be specified with major and minor versions, or major version only.",
							   &default_icu_version,
							   "",
							   PGC_SUSET,
							   0, check_string_version, assign_default_version, NULL);
	DefineCustomBoolVariable("icu_multilib.include_builtin",
							 "Include built-in ICU library when listing or searching libraries.",
							 NULL,
							 &include_builtin,
							 true,
							 PGC_SUSET,
							 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("icu_multilib.search_by_collator_version",
							 "Enable searching for the ICU library based on the collator version.",
							 NULL,
							 &search_by_collator_version,
							 false,
							 PGC_SUSET,
							 0, NULL, NULL, NULL);
}

static bool
parse_icu_version(const char *version, icu_version *out_version)
{
	size_t	input_len = strlen(version);
	int		parse_len;
	int		parse_major;
	int		parse_minor;

	if (sscanf(version, "%d.%d%n",
			   &parse_major, &parse_minor, &parse_len) == 2 &&
		input_len == parse_len)
	{
		out_version->major = parse_major;
		out_version->minor = parse_minor;
		return true;
	}
	else if (sscanf(version, "%d%n", &parse_major, &parse_len) == 1 &&
			 input_len == parse_len)
	{
		out_version->major = parse_major;
		out_version->minor = -1;
		return true;
	}

	return false;
}

static bool
check_string_version(char **newval, void **extra, GucSource source)
{
	pg_icu_library	*lib;
	icu_version		*myextra;
	icu_version		 parsed_version;

	if (*newval == NULL || *newval[0] == '\0')
		return true;

	if (!parse_icu_version(*newval, &parsed_version))
	{
		GUC_check_errmsg("error parsing ICU version \"%s\"", *newval);
		GUC_check_errhint("The ICU version should be either in the form \"major.minor\", "
						  "or just be a plain major version number.");
		return false;
	}

	if (parsed_version.major < PG_MIN_ICU_MAJOR ||
		parsed_version.major > PG_MAX_ICU_MAJOR)
	{
		GUC_check_errmsg("major version %d is out of range",
						 parsed_version.major);
		GUC_check_errhint("ICU major version must be between %d and %d.",
						  PG_MIN_ICU_MAJOR, PG_MAX_ICU_MAJOR);
		return false;
	}

	lib = get_icu_by_major_version(parsed_version.major);
	if (!lib)
	{
		GUC_check_errmsg("ICU library with major version %d not found",
						 parsed_version.major);
		return false;
	}

	if (parsed_version.minor != -1 && lib->minor_version != parsed_version.minor)
	{
		GUC_check_errmsg("ICU library with major version %d has minor version %d; "
						 "expected minor version %d",
						 lib->major_version, lib->minor_version,
						 parsed_version.minor);
		return false;
	}

	myextra = guc_malloc(ERROR, sizeof(icu_version));
	*myextra = parsed_version;
	*extra = myextra;

	return true;
}

static void
assign_default_version(const char *newval, void *extra)
{
	icu_version *myextra = (icu_version *) extra;

	if (myextra == NULL)
		return;

	default_version = *myextra;
}

static void
load_all_libraries()
{
	pg_icu_library *builtin = get_builtin_icu_library();

	icu_library_table[ICU_LIB_TABLESIZE - 1] = builtin;

	ereport(LOG, (errmsg("icu_multilib: retrieved built-in ICU version %d.%d",
						 builtin->major_version, builtin->minor_version)));

	for (int major = PG_MAX_ICU_MAJOR; major >= PG_MIN_ICU_MAJOR; major--)
		icu_library_table[major - PG_MIN_ICU_MAJOR] = load_icu_library(major);
}

static pg_icu_library *
get_icu_by_major_version(int major)
{
	pg_icu_library *lib;
	if (major < PG_MIN_ICU_MAJOR || major > PG_MAX_ICU_MAJOR)
		ereport(ERROR, (errmsg("major version %d is out of range", major)));

	lib = icu_library_table[major - PG_MIN_ICU_MAJOR];
	return lib;
}

static void
get_library_version(const pg_icu_library *lib, int *major, int *minor)
{
	UVersionInfo version_info;
	lib->getICUVersion(version_info);
	*major = version_info[0];
	*minor = version_info[1];
	return;
}

/*
 * Fill in out_version (which must have U_MAX_VERSION_STRING_LENGTH bytes
 * available) with the collator version of the given locale in the given
 * library. Return false if the collator is not found.
 */
static bool
lib_collator_version(pg_icu_library *lib, const char *locale,
					 char *out_version)
{
	UCollator		*collator;
	UVersionInfo	 version_info;
	UErrorCode		 status;

	status = U_ZERO_ERROR;
	collator = lib->openCollator(locale, &status);

	if (!collator)
		return false;

	lib->getCollatorVersion(collator, version_info);
	lib->versionToString(version_info, out_version);
	lib->closeCollator(collator);

	return true;
}

/*
 * Find the right ICU library for the given locale and version. The resulting
 * library may or may not provide a collator with an exactly-matching
 * version.
 *
 * If search_by_collator_version is set, scan the table (first the built-in
 * ICU library, then descending order of major versions) to find the first
 * library that provides a collator of the given locale with a matching
 * version.
 *
 * If no exactly matching version is found, and default_major_version is set,
 * return the default library.
 *
 * Otherwise fall back to the built-in library.
 */
static pg_icu_library *
icu_multilib_hook(Oid collid, const char *locale,
				  const char *requested_version)
{
	return icu_multilib_search(collid, locale, requested_version, true);
}

/*
 *
 */
static pg_icu_library *
icu_multilib_search(Oid collid, const char *locale,
					const char *requested_version, bool logOk)
{
	char			 actual_version[U_MAX_VERSION_STRING_LENGTH];
	pg_icu_library	*found_lib = NULL;

	/*
	 * If another hook was set first, defer to that unless it returns NULL or
	 * a library that doesn't contain the given collator at all. This may
	 * result in a mismatching collator version, but we don't want to
	 * speculate about what's better or worse in the presence of other hooks.
	 */
	if (prev_icu_library_hook)
	{
		pg_icu_library *tmp_lib;
		tmp_lib = prev_icu_library_hook(collid, locale, requested_version);
		if (tmp_lib && lib_collator_version(tmp_lib, locale, actual_version))
			found_lib = tmp_lib;
	}

	if (!found_lib && search_by_collator_version && requested_version != NULL)
	{
		/*
		 * Search from newest library to oldest for a matching version of the
		 * collator with the given name.
		 */
		for (int i = ICU_LIB_TABLESIZE - 1; i >= 0; i--)
		{
			char			 tmp_version[U_MAX_VERSION_STRING_LENGTH];
			pg_icu_library	*tmp_lib = icu_library_table[i];

			if (tmp_lib == NULL)
				continue;

			if (!include_builtin && i == ICU_LIB_TABLESIZE - 1)
				continue;

			if (lib_collator_version(tmp_lib, locale, tmp_version) &&
				strcmp(requested_version, tmp_version) == 0)
			{
				strcpy(actual_version, tmp_version);
				found_lib = tmp_lib;
				break;
			}
		}
	}

	if (!found_lib && default_version.major != -1)
	{
		pg_icu_library *tmp_lib;
		tmp_lib = get_icu_by_major_version(default_version.major);
		if (!tmp_lib)
			ereport(ERROR,
					(errmsg("icu_multilib: default major version %d not found",
							default_version.major)));
		if (lib_collator_version(tmp_lib, locale, actual_version))
			found_lib = tmp_lib;
		else if (logOk)
			ereport(library_search_log_level,
					(errmsg("icu_multilib: found default ICU %d.%d, but collator \"%s\" not found",
							tmp_lib->major_version, tmp_lib->minor_version, locale)));
	}

	if (!found_lib && include_builtin)
	{
		pg_icu_library *tmp_lib;
		tmp_lib = icu_library_table[ICU_LIB_TABLESIZE - 1];
		if (lib_collator_version(tmp_lib, locale, actual_version))
			found_lib = tmp_lib;
		else if (logOk)
			ereport(library_search_log_level,
					(errmsg("icu_multilib: found built-in ICU %d.%d, but collator \"%s\" not found",
							tmp_lib->major_version, tmp_lib->minor_version, locale)));
	}

	/* if not found, fall back to built-in or other hook */
	if (!found_lib)
		return NULL;

	if (logOk)
		ereport(library_search_log_level,
				(errmsg("icu_multilib: found ICU version %d.%d providing collator version \"%s\" for locale \"%s\"",
						found_lib->major_version, found_lib->minor_version,
						actual_version, locale)));

	/*
	 * This is somewhat redundant with a similar warning in pg_locale.c, but
	 * it provides details about the locale name and ICU version, which is
	 * helpful when multiple ICU libraries are in use.
	 */
	if (requested_version && logOk &&
		strcmp(requested_version, actual_version) != 0)
	{
		ereport(version_mismatch_log_level,
				(errmsg("icu_multilib: collator version mismatch detected for locale \"%s\"",
						locale),
				 errdetail("ICU %d.%d provides collator version \"%s\" for locale \"%s\"; expected version \"%s\".",
						   found_lib->major_version, found_lib->minor_version,
						   actual_version, locale, requested_version)));
	}

	return found_lib;
}

PG_FUNCTION_INFO_V1(library_versions);
Datum
library_versions(PG_FUNCTION_ARGS)
{
#define LIBRARY_VERSIONS_COLS 5
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum           values[LIBRARY_VERSIONS_COLS];
	bool            nulls[LIBRARY_VERSIONS_COLS];

	InitMaterializedSRF(fcinfo, 0);

	for (int i = ICU_LIB_TABLESIZE - 1; i >= 0; i--)
	{
		UErrorCode      status;
		UVersionInfo version_info;
		char            version_string[U_MAX_VERSION_STRING_LENGTH];
		pg_icu_library	*lib = icu_library_table[i];

		if (lib == NULL)
			continue;

		if (!include_builtin && i == ICU_LIB_TABLESIZE - 1)
			continue;

		lib->getICUVersion(version_info);
		lib->versionToString(version_info, version_string);
		values[0] = PointerGetDatum(cstring_to_text(version_string));
		nulls[0] = false;

		lib->getUnicodeVersion(version_info);
		lib->versionToString(version_info, version_string);
		values[1] = PointerGetDatum(cstring_to_text(version_string));
		nulls[1] = false;
		status = U_ZERO_ERROR;
		lib->getCLDRVersion(version_info, &status);
		if (U_SUCCESS(status))
		{
			lib->versionToString(version_info, version_string);
			values[2] = PointerGetDatum(cstring_to_text(version_string));
			nulls[2] = false;
		}
		else
		{
			nulls[2] = true;
		}

		values[3] = PointerGetDatum(cstring_to_text(lib->libicui18n_name));
		nulls[3] = false;

		values[4] = PointerGetDatum(cstring_to_text(lib->libicuuc_name));
		nulls[4] = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

#define COLLATOR_DETAIL_COLS 3
static void
make_collator_detail_record(pg_icu_library *lib, const char *locale,
							Datum *values, bool *nulls)
{
	UErrorCode		 status;
	UCollator		*collator;
	UVersionInfo	 version_info;
	char			 version_string[U_MAX_VERSION_STRING_LENGTH];

	status = U_ZERO_ERROR;
	collator = lib->openCollator(locale, &status);

	lib->getICUVersion(version_info);
	lib->versionToString(version_info, version_string);
	values[0] = PointerGetDatum(cstring_to_text(version_string));
	nulls[0] = false;

	if (collator)
	{
		lib->getUCAVersion(collator, version_info);
		lib->versionToString(version_info, version_string);
		values[1] = PointerGetDatum(cstring_to_text(version_string));
		nulls[1] = false;
	}
	else
		nulls[1] = true;

	if (collator)
	{
		lib->getCollatorVersion(collator, version_info);
		lib->versionToString(version_info, version_string);
		values[2] = PointerGetDatum(cstring_to_text(version_string));
		nulls[2] = false;
	}
	else
		nulls[2] = true;

	if (collator)
		lib->closeCollator(collator);

	return;
}

PG_FUNCTION_INFO_V1(library_collators);
Datum
library_collators(PG_FUNCTION_ARGS)
{
#define LIBRARY_COLLATORS_COLS (COLLATOR_DETAIL_COLS + 1)
	pg_icu_library	*lib  = NULL;
	ReturnSetInfo	*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum			 values[LIBRARY_COLLATORS_COLS];
	bool			 nulls[LIBRARY_COLLATORS_COLS];

	if (PG_ARGISNULL(0))
	{
		lib = icu_library_table[ICU_LIB_TABLESIZE - 1];
	}
	else
	{
		char *major_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
		int major = atoi(major_str); /* TODO? */

		if (major < PG_MIN_ICU_MAJOR || major > PG_MAX_ICU_MAJOR)
			ereport(ERROR, (errmsg("icu_multilib: major version %d is out of range", major)));
		lib = get_icu_by_major_version(major);
		if (!lib)
			ereport(ERROR, (errmsg("icu_multilib: major version %d not found", major)));
	}

	InitMaterializedSRF(fcinfo, 0);

	for (int i = -1; i < lib->countAvailable(); i++)
	{
		const char	*name;
		const char	*locale;
		UErrorCode	 status;
		char		 langtag[ULOC_FULLNAME_CAPACITY];

		if (i == -1)
			name = "";		/* ICU root locale */
		else
			name = lib->getAvailable(i);

		status = U_ZERO_ERROR;
		lib->toLanguageTag(name, langtag, sizeof(langtag), true, &status);

		if (U_FAILURE(status))
			continue;

		locale = U_ICU_VERSION_MAJOR_NUM >= 54 ? langtag : name;

		if (!pg_is_ascii(langtag) || !pg_is_ascii(locale))
			continue;

		values[0] = PointerGetDatum(cstring_to_text(locale));
		nulls[0] = false;

		make_collator_detail_record(lib, locale, values + 1, nulls + 1);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(collator_version_search);
Datum
collator_version_search(PG_FUNCTION_ARGS)
{
#define COLLATOR_VERSION_SEARCH_COLS COLLATOR_DETAIL_COLS
	const char		*locale;
	const char		*requested_version = NULL;
	bool			 logOk;
	pg_icu_library	*lib;
	int				 major, minor;
	TupleDesc		 tupdesc;
	HeapTuple		 tuple;
	Datum			 values[COLLATOR_VERSION_SEARCH_COLS];
	bool			 nulls[COLLATOR_VERSION_SEARCH_COLS];

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (PG_ARGISNULL(0))
		ereport(ERROR, (errmsg("locale argument must be non-NULL")));
	if (PG_ARGISNULL(2))
		ereport(ERROR, (errmsg("log_ok argument must be non-NULL")));

	locale = text_to_cstring(PG_GETARG_TEXT_PP(0));
	if (!PG_ARGISNULL(1))
		requested_version = text_to_cstring(PG_GETARG_TEXT_PP(1));
	logOk = PG_GETARG_BOOL(2);

	lib = icu_multilib_search(InvalidOid, locale, requested_version, logOk);
	get_library_version(lib, &major, &minor);

	make_collator_detail_record(lib, locale, values, nulls);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

PG_FUNCTION_INFO_V1(collator_versions);
Datum
collator_versions(PG_FUNCTION_ARGS)
{
#define COLLATOR_VERSIONS_COLS COLLATOR_DETAIL_COLS
	const char *locale;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum           values[COLLATOR_VERSIONS_COLS];
	bool            nulls[COLLATOR_VERSIONS_COLS];

	if (PG_ARGISNULL(0))
		ereport(ERROR, (errmsg("locale argument must be non-NULL")));
	locale = text_to_cstring(PG_GETARG_TEXT_PP(0));

	InitMaterializedSRF(fcinfo, 0);

	for (int i = ICU_LIB_TABLESIZE - 1; i >= 0; i--)
	{
		pg_icu_library	*lib = icu_library_table[i];

		if (lib == NULL)
			continue;

		if (!include_builtin && i == ICU_LIB_TABLESIZE - 1)
			continue;

		make_collator_detail_record(lib, locale, values, nulls);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

static void
make_icu_library_names(int major, char **libicui18n, char **libicuuc)
{
	char libicui18n_tmp[MAXPGPATH];
	char libicuuc_tmp[MAXPGPATH];

	/*
	 * See
	 * https://unicode-org.github.io/icu/userguide/icu4c/packaging.html#icu-versions
	 * for conventions on library naming on POSIX and Windows systems.  Apple
	 * isn't mentioned but varies in the usual way.
	 *
	 * Format 1 is expected to be a major version-only symlink pointing to a
	 * specific minor version (or on Windows it may be the actual library).
	 * Format 2 is expected to be an actual library.
	 */
#ifdef WIN32
#define ICU_LIBRARY_NAME_FORMAT1 "%s%sicu%s%d" DLSUFFIX
#elif defined(__darwin__)
#define ICU_LIBRARY_NAME_FORMAT1 "%s%slibicu%s.%d" DLSUFFIX
#else
#define ICU_LIBRARY_NAME_FORMAT1 "%s%slibicu%s" DLSUFFIX ".%d"
#endif

#ifdef WIN32
#define PATH_SEPARATOR "\\"
#define ICU_I18N "in"
#define ICU_UC "uc"
#else
#define PATH_SEPARATOR "/"
#define ICU_I18N "i18n"
#define ICU_UC "uc"
#endif

	snprintf(libicui18n_tmp,
			 MAXPGPATH,
			 ICU_LIBRARY_NAME_FORMAT1,
			 icu_library_path, icu_library_path[0] ? PATH_SEPARATOR : "",
			 "i18n", major);
	snprintf(libicuuc_tmp,
			 MAXPGPATH,
			 ICU_LIBRARY_NAME_FORMAT1,
			 icu_library_path, icu_library_path[0] ? PATH_SEPARATOR : "",
			 "uc", major);

	*libicui18n = pstrdup(libicui18n_tmp);
	*libicuuc = pstrdup(libicuuc_tmp);
}

#define MAX_SYMBOL_LEN 128
static void *
load_icu_function(void *handle, const char *function, int major)
{
	char     function_with_version[MAX_SYMBOL_LEN];
	int		 ret;
	void    *result;

	/*
	 * Try to look up the symbol with the library major version as a suffix.
	 */
	ret = snprintf(function_with_version, sizeof(function_with_version),
				   "%s_%d", function, major);
	if (ret >= sizeof(function_with_version))
		elog(ERROR, "icu_multilib: cannot construct symbol name");
	result = dlsym(handle, function_with_version);

	/*
	 * Library may have been configured with --disable-renaming, try without
	 * major version suffix.
	 */
	if (result == NULL)
		result = dlsym(handle, function);

	return result;
}

#define LOAD_FUNC(DEST, LIBHANDLE, LIBNAME, FNAME)	\
	do {										\
		DEST = load_icu_function((LIBHANDLE), (FNAME), major);	\
		if (DEST == NULL)						\
		{										\
			ereport(WARNING,					\
					(errmsg("icu_multilib: could not find symbol \"%s\" in library \"%s\"",	\
							(FNAME), (LIBNAME))));	\
			goto err;							\
		}										\
	} while(0)

#define LOAD_FUNC_I18N(DEST, FNAME) \
	LOAD_FUNC(DEST, libicui18n_handle, libicui18n, FNAME)
#define LOAD_FUNC_UC(DEST, FNAME) \
	LOAD_FUNC(DEST, libicuuc_handle, libicuuc, FNAME)

static pg_icu_library *
load_icu_library(int major)
{
	UVersionInfo	 version_info;
	pg_icu_library	*lib;
	char			*libicui18n;
	char			*libicuuc;
	void			*libicui18n_handle = NULL;
	void			*libicuuc_handle   = NULL;

	make_icu_library_names(major, &libicui18n, &libicuuc);

	lib = MemoryContextAllocZero(TopMemoryContext, sizeof(pg_icu_library));

	libicui18n_handle = dlopen(libicui18n, RTLD_NOW | RTLD_LOCAL);
	if (!libicui18n_handle)
		return NULL;

	/* Load the common library. */
	libicuuc_handle = dlopen(libicuuc, RTLD_NOW | RTLD_LOCAL);
	if (!libicui18n_handle)
	{
		elog(WARNING, "found ICU library \"%s\" but not companion library \"%s\"",
			 libicui18n, libicuuc);
		dlclose(libicui18n_handle);
		return NULL;
	}

	/*
	 * We only allocate the pg_icu_library object after successfully
	 * opening the libraries to minimize the work done in the ENOENT case,
	 * when probing a range of versions.  That means we might need to
	 * clean up on allocation failure.
	 */
	lib = MemoryContextAllocExtended(TopMemoryContext, sizeof(*lib),
									 MCXT_ALLOC_NO_OOM);
	lib->libicui18n_name = MemoryContextAllocExtended(
		TopMemoryContext, strlen(libicui18n) + 1, MCXT_ALLOC_NO_OOM);
	lib->libicuuc_name = MemoryContextAllocExtended(
		TopMemoryContext, strlen(libicuuc) + 1, MCXT_ALLOC_NO_OOM);

	if (!lib || !lib->libicui18n_name || !lib->libicuuc_name)
	{
		dlclose(libicui18n_handle);
		dlclose(libicuuc_handle);
		elog(ERROR, "out of memory");
	}

	strcpy(lib->libicui18n_name, libicui18n);
	strcpy(lib->libicuuc_name, libicuuc);

	pfree(libicui18n);
	pfree(libicuuc);

	/* try to find all the symbols we need from the i18n library */
	LOAD_FUNC_I18N(lib->getICUVersion, "u_getVersion");
	LOAD_FUNC_I18N(lib->getUnicodeVersion, "u_getUnicodeVersion");
	LOAD_FUNC_I18N(lib->getCLDRVersion, "ulocdata_getCLDRVersion");
	LOAD_FUNC_I18N(lib->openCollator, "ucol_open");
	LOAD_FUNC_I18N(lib->closeCollator, "ucol_close");
	LOAD_FUNC_I18N(lib->getCollatorVersion, "ucol_getVersion");
	LOAD_FUNC_I18N(lib->getUCAVersion, "ucol_getUCAVersion");
	LOAD_FUNC_I18N(lib->versionToString, "u_versionToString");
	LOAD_FUNC_I18N(lib->strcoll, "ucol_strcoll");
	LOAD_FUNC_I18N(lib->strcollUTF8, "ucol_strcollUTF8");
	LOAD_FUNC_I18N(lib->getSortKey, "ucol_getSortKey");
	LOAD_FUNC_I18N(lib->nextSortKeyPart, "ucol_nextSortKeyPart");
	LOAD_FUNC_I18N(lib->setUTF8, "uiter_setUTF8");
	LOAD_FUNC_I18N(lib->errorName, "u_errorName");
	LOAD_FUNC_I18N(lib->setAttribute, "ucol_setAttribute");

	/* try to find all the symbols we need from the uc library */
	LOAD_FUNC_UC(lib->strToUpper, "u_strToUpper");
	LOAD_FUNC_UC(lib->strToLower, "u_strToLower");
	LOAD_FUNC_UC(lib->strToTitle, "u_strToTitle");
	LOAD_FUNC_UC(lib->openConverter, "ucnv_open");
	LOAD_FUNC_UC(lib->closeConverter, "ucnv_close");
	LOAD_FUNC_UC(lib->fromUChars, "ucnv_fromUChars");
	LOAD_FUNC_UC(lib->toUChars, "ucnv_toUChars");
	LOAD_FUNC_UC(lib->toLanguageTag, "uloc_toLanguageTag");
	LOAD_FUNC_UC(lib->getDisplayName, "uloc_getDisplayName");
	LOAD_FUNC_UC(lib->countAvailable, "uloc_countAvailable");
	LOAD_FUNC_UC(lib->getAvailable, "uloc_getAvailable");

	lib->getICUVersion(version_info);
	lib->major_version = version_info[0];
	lib->minor_version = version_info[1];

	if (lib->major_version != major)
		ereport(version_mismatch_log_level,
				(errmsg("loaded library for major version %d, but library reports major version %d",
						major, lib->major_version)));

	ereport(LOG, (errmsg("icu_multilib: loaded ICU version %d.%d",
						 lib->major_version, lib->minor_version)));

	return lib;

err:
	dlclose(libicui18n_handle);
	dlclose(libicuuc_handle);
	pfree(lib->libicui18n_name);
	pfree(lib->libicuuc_name);
	pfree(lib);
	return NULL;
}
