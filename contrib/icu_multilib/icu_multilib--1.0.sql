/* contrib/icu_multilib/icu_multilib--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION icu_multilib" to load this file. \quit

CREATE FUNCTION icu_library_versions(
    icu_version OUT TEXT,
    unicode_version OUT TEXT,
    cldr_version OUT TEXT,
    libicui18n_name OUT TEXT,
    libicuuc_name OUT TEXT
  )
  RETURNS SETOF RECORD LANGUAGE C
  AS 'MODULE_PATHNAME', 'library_versions';

CREATE FUNCTION collation_version_search (
    locale IN TEXT,
    requested_version IN TEXT,
    log_ok IN BOOLEAN DEFAULT FALSE,
    icu_version OUT TEXT,
    uca_version OUT TEXT,
    collator_version OUT TEXT)
  RETURNS RECORD LANGUAGE C
  AS 'MODULE_PATHNAME', 'collation_version_search';

CREATE FUNCTION collation_versions (
    locale IN TEXT,
    icu_version OUT TEXT,
    uca_version OUT TEXT,
    collator_version OUT TEXT
  )
  RETURNS SETOF RECORD LANGUAGE C
  AS 'MODULE_PATHNAME', 'collation_versions';
