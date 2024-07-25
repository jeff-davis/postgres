CREATE SCHEMA stats_import;

CREATE TYPE stats_import.complex_type AS (
    a integer,
    b real,
    c text,
    d date,
    e jsonb);

CREATE TABLE stats_import.test(
    id INTEGER PRIMARY KEY,
    name text,
    comp stats_import.complex_type,
    arange int4range,
    tags text[]
);

-- starting stats
SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- error: regclass not found
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 0::Oid,
        relpages => 17::integer,
        reltuples => 400.0::real,
        relallvisible => 4::integer);

-- error: relpages NULL
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 'stats_import.test'::regclass,
        relpages => NULL::integer,
        reltuples => 400.0::real,
        relallvisible => 4::integer);

-- error: reltuples NULL
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 'stats_import.test'::regclass,
        relpages => 17::integer,
        reltuples => NULL::real,
        relallvisible => 4::integer);

-- error: relallvisible NULL
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 'stats_import.test'::regclass,
        relpages => 17::integer,
        reltuples => 400.0::real,
        relallvisible => NULL::integer);

-- named arguments
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 'stats_import.test'::regclass,
        relpages => 17::integer,
        reltuples => 400.0::real,
        relallvisible => 4::integer);

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- positional arguments
SELECT
    pg_catalog.pg_set_relation_stats(
        'stats_import.test'::regclass,
        18::integer,
        401.0::real,
        5::integer);

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

DROP SCHEMA stats_import CASCADE;
