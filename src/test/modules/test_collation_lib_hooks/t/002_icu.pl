# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{with_icu} ne 'yes')
{
	plan skip_all => 'ICU not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'test_collation_lib_hooks'
});
$node->start;

# setup
$node->safe_psql('postgres',
	qq[CREATE COLLATION test_asc (PROVIDER=icu, LOCALE='ASC')]);
$node->safe_psql('postgres',
	qq[CREATE COLLATION test_desc (PROVIDER=icu, LOCALE='DESC')]);

$node->safe_psql('postgres', qq[CREATE TABLE strings(t text)]);
$node->safe_psql('postgres',
	qq[INSERT INTO strings VALUES ('aBcD'), ('fGhI'), ('wXyZ')]);

# check versions

my $version_asc =
  $node->safe_psql('postgres',
	  qq[SELECT collversion FROM pg_collation WHERE collname='test_asc']);
is($version_asc, '2.72',
	'collation "test_asc" has correct version 2.72'
);

my $version_desc =
  $node->safe_psql('postgres',
	  qq[SELECT collversion FROM pg_collation WHERE collname='test_desc']);
is($version_desc, '2.72',
	'collation "test_desc" has correct version 2.72'
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
	  qq[SELECT 'aBcD' COLLATE test_asc < 'wXyZ' COLLATE test_asc]);
is($comparison, 't',
	'correct comparison'
);

# test reverse comparison

my $comparison_reverse =
  $node->safe_psql('postgres',
	  qq[SELECT 'aBcD' COLLATE test_desc < 'wXyZ' COLLATE test_desc]);
is($comparison_reverse, 'f',
	'correct reverse comparison'
);

# test asc sort

my $res_sort =
  $node->safe_psql('postgres',
	  qq[SELECT t FROM strings ORDER BY t COLLATE test_asc]);
is($res_sort, $res_sort_expected,
	'correct ascending sort'
);

# test desc sort

my $res_reversesort =
  $node->safe_psql('postgres',
	  qq[SELECT t FROM strings ORDER BY t COLLATE test_desc]);
is($res_reversesort, $res_reversesort_expected,
	'correct descending sort'
);

# test lower/upper

my $tcase =
  $node->safe_psql('postgres',
	  qq[SELECT lower('aBcDfgHiwXyZ' collate test_asc),
                upper('aBcDfgHiwXyZ' collate test_asc)]);
is($tcase, 'abcdfghiwxyz|ABCDFGHIWXYZ',
	'correct lowercase and uppercase'
);

# test reverse lower/upper

my $tcase_reverse =
  $node->safe_psql('postgres',
	  qq[SELECT lower('aBcDfgHiwXyZ' collate test_desc),
                upper('aBcDfgHiwXyZ' collate test_desc)]);
is($tcase_reverse, 'ABCDFGHIWXYZ|abcdfghiwxyz',
	'correct reverse lowercase and uppercase'
);

$node->stop;
done_testing();
