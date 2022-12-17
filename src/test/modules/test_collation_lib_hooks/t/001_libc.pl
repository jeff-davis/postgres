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
	qq[CREATE COLLATION test_reversecase
			  (PROVIDER=libc, LC_COLLATE='ASC', LC_CTYPE='DESC')]);
$node->safe_psql('postgres',
	qq[CREATE COLLATION test_reversesort
			  (PROVIDER=libc, LC_COLLATE='DESC', LC_CTYPE='ASC')]);

$node->safe_psql('postgres', qq[CREATE TABLE strings(t text)]);
$node->safe_psql('postgres',
	qq[INSERT INTO strings VALUES ('aBcD'), ('fGhI'), ('wXyZ')]);

# check versions

my $version_asc =
  $node->safe_psql('postgres',
	  qq[SELECT collversion FROM pg_collation WHERE collname='test_reversecase']);
is($version_asc, '3.14159',
	'collation "test_reversecase" has correct version 3.14159'
);

my $version_desc =
  $node->safe_psql('postgres',
	  qq[SELECT collversion FROM pg_collation WHERE collname='test_reversesort']);
is($version_desc, '3.14159',
	'collation "test_reversesort" has correct version 3.14159'
);

my $res_sort_expected = "aBcD
fGhI
wXyZ";

my $res_reversesort_expected = "wXyZ
fGhI
aBcD";

# test comparison

my $comparison =
  $node->safe_psql('postgres',
	  qq[SELECT 'aBcD' COLLATE test_reversecase < 'wXyZ' COLLATE test_reversecase]);
is($comparison, 't',
	'correct comparison'
);

# test reverse comparison

my $comparison_reverse =
  $node->safe_psql('postgres',
	  qq[SELECT 'aBcD' COLLATE test_reversesort < 'wXyZ' COLLATE test_reversesort]);
is($comparison_reverse, 'f',
	'correct reverse comparison'
);

# test asc sort with trust_strxfrm = false

my $res_sort =
  $node->safe_psql('postgres',
	  qq[SET trust_strxfrm = false;
         SELECT t FROM strings ORDER BY t COLLATE test_reversecase]);
is($res_sort, $res_sort_expected,
	'correct ascending sort (trust_strxfrm = false)'
);

# test desc sort with trust_strxfrm = false

my $res_reversesort =
  $node->safe_psql('postgres',
	  qq[SET trust_strxfrm = false;
         SELECT t FROM strings ORDER BY t COLLATE test_reversesort]);
is($res_reversesort, $res_reversesort_expected,
	'correct descending sort (trust_strxfrm = false)'
);

# test asc sort with trust_strxfrm = true

my $res_sort_strxfrm =
  $node->safe_psql('postgres',
	  qq[SET trust_strxfrm = true;
         SELECT t FROM strings ORDER BY t COLLATE test_reversecase]);
is($res_sort_strxfrm, $res_sort_expected,
	'correct ascending sort (trust_strxfrm = true)'
);

# test desc sort with trust_strxfrm = true

my $res_reversesort_strxfrm =
  $node->safe_psql('postgres',
	  qq[SET trust_strxfrm = true;
         SELECT t FROM strings ORDER BY t COLLATE test_reversesort]);
is($res_reversesort_strxfrm, $res_reversesort_expected,
	'correct descending sort (trust_strxfrm = true)'
);

# test lower/upper

my $tcase =
  $node->safe_psql('postgres',
	  qq[SELECT lower('aBcDfgHiwXyZ' collate test_reversesort),
                upper('aBcDfgHiwXyZ' collate test_reversesort)]);
is($tcase, 'abcdfghiwxyz|ABCDFGHIWXYZ',
	'correct lowercase and uppercase'
);

# test reverse lower/upper

my $tcase_reverse =
  $node->safe_psql('postgres',
	  qq[SELECT lower('aBcDfgHiwXyZ' collate test_reversecase),
                upper('aBcDfgHiwXyZ' collate test_reversecase)]);
is($tcase_reverse, 'ABCDFGHIWXYZ|abcdfghiwxyz',
	'correct lowercase and uppercase'
);



$node->stop;
done_testing();
