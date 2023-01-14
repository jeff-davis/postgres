/* contrib/icu_multilib/icu_multilib--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION icu_multilib" to load this file. \quit

CREATE FUNCTION library_versions(
    icu_version OUT TEXT,
    unicode_version OUT TEXT,
    cldr_version OUT TEXT,
    libicui18n_name OUT TEXT,
    libicuuc_name OUT TEXT
  )
  RETURNS SETOF RECORD LANGUAGE C
  AS 'MODULE_PATHNAME', 'library_versions';

CREATE FUNCTION library_collators(
    major_version IN TEXT DEFAULT NULL,
    locale OUT TEXT,
    icu_version OUT TEXT,
    uca_version OUT TEXT,
    collator_version OUT TEXT
  )
  RETURNS SETOF RECORD LANGUAGE C
  AS 'MODULE_PATHNAME', 'library_collators';

CREATE FUNCTION collator_version_search (
    locale IN TEXT,
    requested_version IN TEXT DEFAULT NULL,
    log_ok IN BOOLEAN DEFAULT FALSE,
    icu_version OUT TEXT,
    uca_version OUT TEXT,
    collator_version OUT TEXT)
  RETURNS RECORD LANGUAGE C
  AS 'MODULE_PATHNAME', 'collator_version_search';

CREATE FUNCTION collator_versions (
    locale IN TEXT,
    icu_version OUT TEXT,
    uca_version OUT TEXT,
    collator_version OUT TEXT
  )
  RETURNS SETOF RECORD LANGUAGE C
  AS 'MODULE_PATHNAME', 'collator_versions';
