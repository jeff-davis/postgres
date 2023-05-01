/*
 * This test is for collations and character operations when using the
 * builtin provider with the C.UTF-8 locale.
 */

/* skip test if not UTF8 server encoding */
SELECT getdatabaseencoding() <> 'UTF8' AS skip_test \gset
\if :skip_test
\quit
\endif

SET client_encoding TO UTF8;

--
-- Test builtin provider with C.UTF-8 locale.
--
CREATE COLLATION builtin_c_utf8 (PROVIDER = builtin, LOCALE = 'C.UTF-8');

CREATE TABLE builtin_test (
  t TEXT COLLATE builtin_c_utf8
);
INSERT INTO builtin_test VALUES
  ('abc DEF'),
  ('Ǆxxx ǅxxx ǆxxx'),
  ('xxxǄ xxxǅ xxxǆ');

SELECT t, lower(t), initcap(t), upper(t) FROM builtin_test;

DROP TABLE builtin_test;

-- character classes

select 'xyz' ~ '[[:alnum:]]' collate builtin_c_utf8;
select 'xyz' !~ '[[:upper:]]' collate builtin_c_utf8;
select '@' !~ '[[:alnum:]]' collate builtin_c_utf8;
select '@' ~ '[[:punct:]]' collate builtin_c_utf8; -- symbols are punctuation in posix
select 'a8a' ~ '[[:digit:]]' collate builtin_c_utf8;
select '൧' !~ '\d' collate builtin_c_utf8; -- only 0-9 considered digits in posix

-- case mapping

select 'xYz' ~* 'XyZ' collate builtin_c_utf8;
select 'xAb' ~* '[W-Y]' collate builtin_c_utf8;
select 'xAb' !~* '[c-d]' collate builtin_c_utf8;
select 'Δ' ~* '[α-λ]' collate builtin_c_utf8;

DROP COLLATION builtin_c_utf8;
