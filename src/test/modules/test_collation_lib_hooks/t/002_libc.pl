# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'test_collation_lib_hooks'
});
$node->start;

# setup
$node->safe_psql('postgres',
	qq[CREATE COLLATION test_asc (PROVIDER=libc, LOCALE='ASC')]);
$node->safe_psql('postgres',
	qq[CREATE COLLATION test_desc (PROVIDER=libc, LOCALE='DESC')]);

$node->safe_psql('postgres', qq[CREATE TABLE strings(t text)]);
$node->safe_psql('postgres',
	qq[INSERT INTO strings VALUES ('aBcD'), ('fGhI'), ('wXyZ')]);

my $sort_asc_expected = "aBcD
fGhI
wXyZ";

my $sort_desc_expected = "wXyZ
fGhI
aBcD";

# test comparison

my $comparison_asc =
  $node->safe_psql('postgres',
	  qq[SELECT 'aBcD' COLLATE test_asc < 'wXyZ' COLLATE test_asc]);
is($comparison_asc, 't',
	'correct comparison'
);

# test desc comparison

my $comparison_desc =
  $node->safe_psql('postgres',
	  qq[SELECT 'aBcD' COLLATE test_desc < 'wXyZ' COLLATE test_desc]);
is($comparison_desc, 'f',
	'correct desc comparison'
);

# test asc sort with trust_strxfrm = false

my $sort_asc =
  $node->safe_psql('postgres',
	  qq[SET trust_strxfrm = false;
         SELECT t FROM strings ORDER BY t COLLATE test_asc]);
is($sort_asc, $sort_asc_expected,
	'correct ascending sort (trust_strxfrm = false)'
);

# test desc sort with trust_strxfrm = false

my $sort_desc =
  $node->safe_psql('postgres',
	  qq[SET trust_strxfrm = false;
         SELECT t FROM strings ORDER BY t COLLATE test_desc]);
is($sort_desc, $sort_desc_expected,
	'correct descending sort (trust_strxfrm = false)'
);

# test asc sort with trust_strxfrm = true

my $strxfrm_asc =
  $node->safe_psql('postgres',
	  qq[SET trust_strxfrm = true;
         SELECT t FROM strings ORDER BY t COLLATE test_asc]);
is($strxfrm_asc, $sort_asc_expected,
	'correct ascending sort (trust_strxfrm = true)'
);

# test desc sort with trust_strxfrm = true

my $strxfrm_desc =
  $node->safe_psql('postgres',
	  qq[SET trust_strxfrm = true;
         SELECT t FROM strings ORDER BY t COLLATE test_desc]);
is($strxfrm_desc, $sort_desc_expected,
	'correct descending sort (trust_strxfrm = true)'
);

# test lower/upper

my $tcase =
  $node->safe_psql('postgres',
	  qq[SELECT lower('aBcDfgHiwXyZ' collate test_asc),
                upper('aBcDfgHiwXyZ' collate test_asc)]);
is($tcase, 'abcdfghiwxyz|ABCDFGHIWXYZ',
	'correct lowercase and uppercase'
);

# test desc lower/upper

my $tcase_desc =
  $node->safe_psql('postgres',
	  qq[SELECT lower('aBcDfgHiwXyZ' collate test_desc),
                upper('aBcDfgHiwXyZ' collate test_desc)]);
is($tcase_desc, 'ABCDFGHIWXYZ|abcdfghiwxyz',
	'correct desc lowercase and uppercase'
);

if ($ENV{win32} ne 'yes') {
  $node->safe_psql('postgres',
     qq[CREATE COLLATION test_mixed_asc_desc
         (PROVIDER=libc, LC_COLLATE='ASC', LC_CTYPE='DESC')]);
  $node->safe_psql('postgres',
     qq[CREATE COLLATION test_mixed_desc_asc
         (PROVIDER=libc, LC_COLLATE='DESC', LC_CTYPE='ASC')]);

  my $mcomparison_asc =
    $node->safe_psql('postgres',
	  qq[SELECT 'aBcD' COLLATE test_mixed_asc_desc <
                'wXyZ' COLLATE test_mixed_asc_desc]);
  is($mcomparison_asc, 't',
	'correct mixed asc/desc comparison'
  );

  my $mcomparison_desc =
    $node->safe_psql('postgres',
	  qq[SELECT 'aBcD' COLLATE test_mixed_desc_asc <
                'wXyZ' COLLATE test_mixed_desc_asc]);
  is($mcomparison_desc, 'f',
	'correct mixed desc/asc comparison'
  );

  my $mcase_asc =
    $node->safe_psql('postgres',
	    qq[SELECT lower('aBcDfgHiwXyZ' collate test_mixed_asc_desc),
                  upper('aBcDfgHiwXyZ' collate test_mixed_asc_desc)]);
  is($mcase_asc, 'ABCDFGHIWXYZ|abcdfghiwxyz',
    'correct case mixed asc/desc'
  );

  my $mcase_desc =
    $node->safe_psql('postgres',
	    qq[SELECT lower('aBcDfgHiwXyZ' collate test_mixed_desc_asc),
                  upper('aBcDfgHiwXyZ' collate test_mixed_desc_asc)]);
  is($mcase_desc, 'abcdfghiwxyz|ABCDFGHIWXYZ',
    'correct case mixed desc/asc'
  );
}

$node->stop;
done_testing();
