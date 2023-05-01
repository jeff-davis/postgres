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
-- Test preinstalled C_UTF8 collation.
--

CREATE TABLE builtin_test (
  t TEXT COLLATE C_UTF8
);
INSERT INTO builtin_test VALUES
  ('abc DEF'),
  ('ábc DÉF'),
  ('ǄxxǄ ǆxxǅ ǅxxǆ');

SELECT t, lower(t), initcap(t), upper(t) FROM builtin_test;

DROP TABLE builtin_test;

-- character classes

SELECT 'xyz' ~ '[[:alnum:]]' COLLATE C_UTF8;
SELECT 'xyz' !~ '[[:upper:]]' COLLATE C_UTF8;
SELECT '@' !~ '[[:alnum:]]' COLLATE C_UTF8;
SELECT '@' ~ '[[:punct:]]' COLLATE C_UTF8; -- symbols are punctuation in posix
SELECT 'a8a' ~ '[[:digit:]]' COLLATE C_UTF8;
SELECT '൧' !~ '\d' COLLATE C_UTF8; -- only 0-9 considered digits in posix

-- case mapping

SELECT 'xYz' ~* 'XyZ' COLLATE C_UTF8;
SELECT 'xAb' ~* '[W-Y]' COLLATE C_UTF8;
SELECT 'xAb' !~* '[c-d]' COLLATE C_UTF8;
SELECT 'Δ' ~* '[α-λ]' COLLATE C_UTF8;
SELECT 'δ' ~* '[Γ-Λ]' COLLATE C_UTF8; -- same as above with cases reversed
