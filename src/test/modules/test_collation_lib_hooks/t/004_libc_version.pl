# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test collation versions (platform-specific)

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $version_asc_expected;
my $version_desc_expected;

if ($ENV{glibc} eq 'yes') {
	$version_asc_expected = '3.14159';
	$version_desc_expected = '3.14159';
} elsif ($ENV{have_lc_version_mask} eq 'yes') {
	$version_asc_expected = '3.14';
	$version_desc_expected = '6.28';
} elsif ($ENV{win32} eq 'yes') {
	$version_asc_expected = '3.14,3.14';
	$version_desc_expected = '6.28,6.28';
} else {
	plan skip_all => 'platform does not support libc collation versions';
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
	qq[CREATE COLLATION test_asc (PROVIDER=libc, LOCALE='ASC')]);
$node->safe_psql('postgres',
	qq[CREATE COLLATION test_desc (PROVIDER=libc, LOCALE='DESC')]);

$node->safe_psql('postgres', qq[CREATE TABLE strings(t text)]);
$node->safe_psql('postgres',
	qq[INSERT INTO strings VALUES ('aBcD'), ('fGhI'), ('wXyZ')]);

# check versions

my $pg_version = $node->safe_psql('postgres', qq[SELECT version()]);

my $version_asc =
  $node->safe_psql('postgres',
	  qq[SELECT collversion FROM pg_collation WHERE collname='test_asc']);
is($version_asc, $version_asc_expected,
	"collation test_asc has correct version $version_asc_expected"
);

my $version_desc =
  $node->safe_psql('postgres',
	  qq[SELECT collversion FROM pg_collation WHERE collname='test_desc']);
is($version_desc, $version_desc_expected,
	"collation test_desc has correct version $version_desc_expected"
);

$node->stop;
done_testing();
