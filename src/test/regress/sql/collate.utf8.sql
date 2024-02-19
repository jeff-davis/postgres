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
-- Test builtin PG_UNICODE_FAST locale.
--

CREATE TABLE builtin_test1 (
  t TEXT COLLATE PG_UNICODE_FAST
);
INSERT INTO builtin_test1 VALUES
  ('abc DEF'),
  ('ábc sßs ßss DÉF'),
  ('ǄxxǄ ǆxxǅ ǅxxǆ'),
  ('ȺȺȺ'),
  ('ⱥⱥⱥ'),
  ('ⱥȺ');

SELECT
    t, lower(t), initcap(t), upper(t),
    length(convert_to(t, 'UTF8')) AS t_bytes,
    length(convert_to(lower(t), 'UTF8')) AS lower_t_bytes,
    length(convert_to(initcap(t), 'UTF8')) AS initcap_t_bytes,
    length(convert_to(upper(t), 'UTF8')) AS upper_t_bytes
  FROM builtin_test1;

DROP TABLE builtin_test1;

-- test Final_Sigma
SELECT lower('ΑΣ' COLLATE PG_UNICODE_FAST); -- 0391 03A3
SELECT lower('ΑΣ0' COLLATE PG_UNICODE_FAST); -- 0391 03A3 0030
SELECT lower('ἈΣ̓' COLLATE PG_UNICODE_FAST); -- 0391 0343 03A3 0343
SELECT lower('ᾼΣͅ' COLLATE PG_UNICODE_FAST); -- 0391 0345 03A3 0345

-- test !Final_Sigma
SELECT lower('Σ' COLLATE PG_UNICODE_FAST); -- 03A3
SELECT lower('0Σ' COLLATE PG_UNICODE_FAST); -- 0030 03A3
SELECT lower('ΑΣΑ' COLLATE PG_UNICODE_FAST); -- 0391 03A3 0391
SELECT lower('ἈΣ̓Α' COLLATE PG_UNICODE_FAST); -- 0391 0343 03A3 0343 0391
SELECT lower('ᾼΣͅΑ' COLLATE PG_UNICODE_FAST); -- 0391 0345 03A3 0345 0391

-- properties

SELECT 'xyz' ~ '[[:alnum:]]' COLLATE PG_UNICODE_FAST;
SELECT 'xyz' !~ '[[:upper:]]' COLLATE PG_UNICODE_FAST;
SELECT '@' !~ '[[:alnum:]]' COLLATE PG_UNICODE_FAST;
SELECT '=' !~ '[[:punct:]]' COLLATE PG_UNICODE_FAST; -- symbols are not punctuation
SELECT 'a8a' ~ '[[:digit:]]' COLLATE PG_UNICODE_FAST;
SELECT '൧' ~ '\d' COLLATE PG_UNICODE_FAST;

-- case mapping

SELECT 'xYz' ~* 'XyZ' COLLATE PG_UNICODE_FAST;
SELECT 'xAb' ~* '[W-Y]' COLLATE PG_UNICODE_FAST;
SELECT 'xAb' !~* '[c-d]' COLLATE PG_UNICODE_FAST;
SELECT 'Δ' ~* '[α-λ]' COLLATE PG_UNICODE_FAST;
SELECT 'δ' ~* '[Γ-Λ]' COLLATE PG_UNICODE_FAST; -- same as above with cases reversed

--
-- Test builtin C.UTF-8 locale.
--

CREATE COLLATION BUILTIN_C_UTF8 ( provider = builtin, locale = 'C.UTF-8' );

CREATE TABLE builtin_test2 (
  t TEXT COLLATE BUILTIN_C_UTF8
);
INSERT INTO builtin_test2 VALUES
  ('abc DEF'),
  ('ábc sßs ßss DÉF'),
  ('ǄxxǄ ǆxxǅ ǅxxǆ'),
  ('ȺȺȺ'),
  ('ⱥⱥⱥ'),
  ('ⱥȺ');

SELECT
    t, lower(t), initcap(t), upper(t),
    length(convert_to(t, 'UTF8')) AS t_bytes,
    length(convert_to(lower(t), 'UTF8')) AS lower_t_bytes,
    length(convert_to(initcap(t), 'UTF8')) AS initcap_t_bytes,
    length(convert_to(upper(t), 'UTF8')) AS upper_t_bytes
  FROM builtin_test2;

DROP TABLE builtin_test2;

-- negative test: Final_Sigma not used for builtin locale C.UTF-8
SELECT lower('ΑΣ' COLLATE BUILTIN_C_UTF8);
SELECT lower('ΑͺΣͺ' COLLATE BUILTIN_C_UTF8);
SELECT lower('Α΄Σ΄' COLLATE BUILTIN_C_UTF8);

-- properties

SELECT 'xyz' ~ '[[:alnum:]]' COLLATE BUILTIN_C_UTF8;
SELECT 'xyz' !~ '[[:upper:]]' COLLATE BUILTIN_C_UTF8;
SELECT '@' !~ '[[:alnum:]]' COLLATE BUILTIN_C_UTF8;
SELECT '=' ~ '[[:punct:]]' COLLATE BUILTIN_C_UTF8; -- symbols are punctuation in posix
SELECT 'a8a' ~ '[[:digit:]]' COLLATE BUILTIN_C_UTF8;
SELECT '൧' !~ '\d' COLLATE BUILTIN_C_UTF8; -- only 0-9 considered digits in posix

-- case mapping

SELECT 'xYz' ~* 'XyZ' COLLATE BUILTIN_C_UTF8;
SELECT 'xAb' ~* '[W-Y]' COLLATE BUILTIN_C_UTF8;
SELECT 'xAb' !~* '[c-d]' COLLATE BUILTIN_C_UTF8;
SELECT 'Δ' ~* '[α-λ]' COLLATE BUILTIN_C_UTF8;
SELECT 'δ' ~* '[Γ-Λ]' COLLATE BUILTIN_C_UTF8; -- same as above with cases reversed

DROP COLLATION BUILTIN_C_UTF8;
