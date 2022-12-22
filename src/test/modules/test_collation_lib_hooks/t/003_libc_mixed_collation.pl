# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test mixed collations with differing lc_collate/lc_ctype

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{win32} eq 'yes')
{
	plan skip_all => 'windows does not support mixed libc collations';
}

my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'test_collation_lib_hooks'
});
$node->start;

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

$node->stop;
done_testing();
