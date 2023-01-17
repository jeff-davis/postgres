/* contrib/icu_multilib/icu_multilib--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION icu_multilib" to load this file. \quit

CREATE FUNCTION icu_multilib.icu_library_versions(
    icu_version OUT TEXT,
    unicode_version OUT TEXT,
    cldr_version OUT TEXT,
    libicui18n_name OUT TEXT,
    libicuuc_name OUT TEXT
  )
  RETURNS SETOF RECORD LANGUAGE C
  AS 'MODULE_PATHNAME', 'icu_library_versions';

CREATE FUNCTION icu_multilib.icu_collation_versions(
    locale IN TEXT,
    icu_version OUT TEXT,
    uca_version OUT TEXT,
    collator_version OUT TEXT,
  )
  RETURNS SETOF RECORD LANGUAGE C
  AS 'MODULE_PATHNAME', 'icu_collation_versions';
