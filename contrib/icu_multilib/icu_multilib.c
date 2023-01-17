
#include "postgres.h"

#include <dlfcn.h>
#include <limits.h>

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

PG_MODULE_MAGIC;

static get_icu_library_hook_type prev_icu_library_hook = NULL;

static pg_icu_library *icu_library_table[ICU_LIB_TABLESIZE] = {};

static char *icu_library_path = "";
static int default_icu_major_version = -1;
static int default_icu_minor_version = -1;
static bool search_by_collversion = true;
static bool include_builtin_icu = true;
static pg_icu_library *default_icu_library = NULL;

static void icu_multilib_gucs(void);
static pg_icu_library *icu_multilib_hook(const char *locale,
										 const char *version);
static void load_all_libraries(void);
static pg_icu_library *load_icu_library(int major);
static pg_icu_library *get_icu_library(int major);
static void get_library_version(const pg_icu_library *lib, int *major,
								int *minor);


void
_PG_init()
{
	/*TODO: error messages */
	if (!process_shared_preload_libraries_in_progress)
		elog(ERROR, "must be loaded with shared_preload_libraries");
	
	prev_icu_library_hook = get_icu_library_hook;
	get_icu_library_hook = icu_multilib_hook;
	icu_multilib_gucs();
	load_all_libraries();

	if (default_icu_major_version > -1)
	{
		default_icu_library = get_icu_library(default_icu_major_version);
		if (default_icu_library == NULL)
			elog(ERROR, "couldn't load default icu library: couldn't find major version %d",
				 default_icu_major_version);

		if (default_icu_minor_version > -1)
		{
			int major, minor;
			get_library_version(default_icu_library, &major, &minor);
			if (minor != default_icu_minor_version)
				elog(ERROR, "default icu library version minor doesn't match");
		}
	}
}

static void
icu_multilib_gucs()
{
	DefineCustomStringVariable("icu_multilib.library_path",
							   "Filesystem path where ICU libraries are installed.",
							   NULL,
							   &icu_library_path,
							   "",
							   PGC_POSTMASTER,
							   0,
							   NULL, NULL, NULL);
	DefineCustomIntVariable("icu_multilib.default_icu_major_version",
							"The major version of the default ICU library.",
							"A setting of -1 means that the built-in ICU is the default.",
							&default_icu_major_version,
							-1,
							-1, PG_MAX_ICU_MAJOR,
							PGC_POSTMASTER,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);
	DefineCustomIntVariable("icu_multilib.default_icu_minor_version",
							"The required minor version of the default ICU library.",
							"A setting of -1 means any minor version is accepted.",
							&default_icu_minor_version,
							-1,
							-1, INT_MAX,
							PGC_POSTMASTER,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);
	DefineCustomBoolVariable("icu_multilib.include_builtin_icu",
							 "Include builtin ICU when listing or searching libraries.",
							 NULL,
							 &include_builtin_icu,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomBoolVariable("icu_multilib.search_by_collversion",
							 "Enable searching for the ICU library based on the collversion.",
							 NULL,
							 &search_by_collversion,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
}

static void
load_all_libraries()
{
	for (int i = 0; i < ICU_LIB_TABLESIZE - 1; i++)
	{
		int major = i + PG_MIN_ICU_MAJOR;
		icu_library_table[major - PG_MIN_ICU_MAJOR] = load_icu_library(major);
	}

	icu_library_table[ICU_LIB_TABLESIZE - 1] = get_builtin_icu_library();
}

static pg_icu_library *
get_icu_library(int major)
{
	Assert(major >= PG_MIN_ICU_MAJOR && major <= PG_MAX_ICU_MAJOR);
	return icu_library_table[major - PG_MIN_ICU_MAJOR];
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
 * Return true if the given library's version of the collation with the given
 * name matches the requested version; false otherwise.
 */
static bool
lib_matching_collversion(const pg_icu_library *lib, const char *locale,
						 const char *requested_version)
{
	UCollator		*collator;
	UVersionInfo	 version_info;
	UErrorCode		 status;
	char			 version_info_string[U_MAX_VERSION_STRING_LENGTH];
	bool			 result = false;

	status = U_ZERO_ERROR;
	collator = lib->openCollator(locale, &status);
	if (!collator)
		return false;

	lib->getCollatorVersion(collator, version_info);
	lib->versionToString(version_info, version_info_string);

	if (strcmp(version_info_string, requested_version) == 0)
		result = true;
	
	lib->closeCollator(collator);

	return result;
}

static pg_icu_library *
icu_multilib_hook(const char *locale, const char *version)
{
	if (search_by_collversion && version != NULL)
	{
		/*
		 * Search from newest library to oldest for a matching version of the
		 * collation with the given name.
		 */
		for (int i = ICU_LIB_TABLESIZE - 1; i >= 0; i--)
		{
			pg_icu_library *lib = icu_library_table[i];

			if (lib == NULL)
				continue;

			if (!include_builtin_icu && i == ICU_LIB_TABLESIZE - 1)
				continue;

			if (lib_matching_collversion(lib, locale, version))
				return lib;
		}
	}

	if (default_icu_library)
		return default_icu_library;

	if (prev_icu_library_hook)
		return prev_icu_library_hook(locale, version);

	/* fall back to builtin */
	return NULL;
}

PG_FUNCTION_INFO_V1(icu_library_versions);
Datum
icu_library_versions(PG_FUNCTION_ARGS)
{
#define PG_ICU_AVAILABLE_ICU_LIRBARIES_COLS 5
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum           values[PG_ICU_AVAILABLE_ICU_LIRBARIES_COLS];
	bool            nulls[PG_ICU_AVAILABLE_ICU_LIRBARIES_COLS];

	InitMaterializedSRF(fcinfo, 0);

	for (int i = ICU_LIB_TABLESIZE - 1; i >= 0; i--)
	{
		UErrorCode      status;
		UVersionInfo version_info;
		char            version_string[U_MAX_VERSION_STRING_LENGTH];
		pg_icu_library	*lib = icu_library_table[i];

		if (lib == NULL)
			continue;

		if (!include_builtin_icu && i == ICU_LIB_TABLESIZE - 1)
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

PG_FUNCTION_INFO_V1(icu_collation_versions);
Datum
icu_collation_versions(PG_FUNCTION_ARGS)
{
#define PG_ICU_COLLATION_VERSION_COLS 3
	const char *locale = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum           values[PG_ICU_COLLATION_VERSION_COLS];
	bool            nulls[PG_ICU_COLLATION_VERSION_COLS];

	InitMaterializedSRF(fcinfo, 0);

	for (int i = ICU_LIB_TABLESIZE - 1; i >= 0; i--)
	{
		UErrorCode		 status;
		UCollator		*collator;
		UVersionInfo	 version_info;
		char			 version_string[U_MAX_VERSION_STRING_LENGTH];
		pg_icu_library	*lib = icu_library_table[i];

		if (lib == NULL)
			continue;

		if (!include_builtin_icu && i == ICU_LIB_TABLESIZE - 1)
			continue;

		status = U_ZERO_ERROR;
		collator = lib->openCollator(locale, &status);
		if (!collator)
		{
			if (U_FAILURE(status))
				ereport(WARNING,
						(errmsg("could not open collator for locale \"%s\" from ICU %d.%d: %s",
								locale,
								lib->major_version,
								lib->minor_version,
								lib->errorName(status))));
			continue;
		}

		lib->getICUVersion(version_info);
		lib->versionToString(version_info, version_string);
		values[0] = PointerGetDatum(cstring_to_text(version_string));
		nulls[0] = false;

		lib->getUCAVersion(collator, version_info);
		lib->versionToString(version_info, version_string);
		values[1] = PointerGetDatum(cstring_to_text(version_string));
		nulls[1] = false;

		lib->getCollatorVersion(collator, version_info);
		lib->versionToString(version_info, version_string);
		values[2] = PointerGetDatum(cstring_to_text(version_string));
		nulls[2] = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

		lib->closeCollator(collator);
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
	Assert(ret < sizeof(function_with_version));
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
			dlclose(libicui18n_handle);			\
			dlclose(libicuuc_handle);			\
			pfree(lib);							\
			ereport(WARNING,					\
					(errmsg("could not find symbol \"%s\" in library \"%s\"",	\
							(FNAME), (LIBNAME))));	\
			return NULL;						\
		}										\
	} while(0)

#define LOAD_FUNC_I18N(DEST, FNAME) \
	LOAD_FUNC(DEST, libicui18n_handle, libicui18n, FNAME)
#define LOAD_FUNC_UC(DEST, FNAME) \
	LOAD_FUNC(DEST, libicuuc_handle, libicuuc, FNAME)

static pg_icu_library *
load_icu_library(int major)
{
	pg_icu_library	*lib;
	char			*libicui18n;
	char			*libicuuc;
	void			*libicui18n_handle = NULL;
	void			*libicuuc_handle = NULL;

	make_icu_library_names(major, &libicui18n, &libicuuc);

	elog(LOG, "loading %s and %s", libicui18n, libicuuc);

	lib = MemoryContextAllocZero(TopMemoryContext, sizeof(pg_icu_library));

	libicui18n_handle = dlopen(libicui18n, RTLD_NOW | RTLD_LOCAL);
	if (!libicui18n_handle)
	{
		elog(LOG, "can't open %s", libicui18n);
		return NULL;
	}

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

	elog(LOG, "loaded major version %d", major);
	return lib;
}
