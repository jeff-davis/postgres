# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that streaming replication can replace corrupt WAL that walreceiver
# previously reported as flushed.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# pack() of xl_prev must match on-disk endianness.  'Q' is not available in
# all Perl builds, so split the 64-bit LSN into two 32-bit fields.
my $BIG_ENDIAN = pack('L', 0x12345678) eq pack('N', 0x12345678);

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
wal_keep_size = 1GB
));
$primary->start;

$primary->backup('backup');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf', qq(
recovery_prefetch = off
wal_retrieve_retry_interval = '100ms'
));
$standby->start;

$primary->wait_for_replay_catchup($standby);

# Pause before generating the WAL that will be corrupted.  Moving to a new
# segment ensures that the startup process has not cached the page containing
# that record.
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');
$standby->poll_query_until(
	'postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "timed out while waiting for recovery to pause";
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');

# Capture the start of an unreplayed record, then stream well past it so
# walreceiver's flushedUpto is ahead of the later corruption.
my $record_start = int(
	$primary->safe_psql(
		'postgres', "SELECT pg_current_wal_insert_lsn() - '0/0'"));
$primary->emit_wal(1024);
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->emit_wal(8192);
my $target_lsn = $primary->lsn('flush');
$primary->wait_for_catchup($standby, 'flush', $target_lsn);

# Stop walreceiver without restarting the postmaster, preserving its
# flushedUpto high-water mark in shared memory.
my $walreceiver_pid = $standby->safe_psql(
	'postgres', 'SELECT pid FROM pg_stat_wal_receiver');
kill 'TERM', $walreceiver_pid
  or die "could not terminate walreceiver $walreceiver_pid: $!";
$standby->poll_query_until(
	'postgres',
	'SELECT NOT EXISTS (SELECT FROM pg_stat_wal_receiver)')
  or die "timed out while waiting for walreceiver to stop";

# Overwrite the unreplayed record header with a plausible but incorrect
# prev-link.  The primary retains the correct copy.
my $wal_segment_size = int(
	$standby->safe_psql(
		'postgres',
		"SELECT setting FROM pg_settings WHERE name = 'wal_segment_size'"));
my $tli = int(
	$standby->safe_psql(
		'postgres', 'SELECT timeline_id FROM pg_control_checkpoint()'));
# XLogRecord: xl_tot_len, xl_xid, xl_prev, xl_info, xl_rmid, pad, xl_crc.
# xl_tot_len=24 and xl_prev=0xdeadbeef fail ValidXLogRecordHeader.
$standby->write_wal(
	$tli, $record_start,
	$wal_segment_size,
	pack(
		'IIIICCBBI',
		24, 0,
		$BIG_ENDIAN ? 0           : 0xdeadbeef,
		$BIG_ENDIAN ? 0xdeadbeef : 0,
		0, 0, 0, 0, 0));

$standby->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');

$standby->poll_query_until(
	'postgres',
	"SELECT pg_last_wal_replay_lsn() >= '$target_lsn'")
  or die "standby did not replace corrupt WAL and catch up";

like(
	slurp_file($standby->logfile),
	qr/record with incorrect prev-link 0\/DEADBEEF/,
	'standby observed the injected corrupt record');

pass('standby replaced corrupt WAL from an earlier streaming position');

$standby->stop;
$primary->stop;

done_testing();
