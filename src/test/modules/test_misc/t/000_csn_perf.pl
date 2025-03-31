
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Verify that ALTER TABLE optimizes certain operations as expected

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);

my $duration = 15; # seconds
my $miniterations = 3;

# Initialize a test cluster
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init();
# Turn message level up to DEBUG1 so that we get the messages we want to see
$primary->append_conf('postgresql.conf', 'max_wal_senders = 5');
$primary->append_conf('postgresql.conf', 'wal_level=replica');
$primary->append_conf('postgresql.conf', 'max_connections = 1005');
$primary->start;
$primary->backup('bkp');

my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->init_from_backup($primary, 'bkp', has_streaming => 1);
$replica->append_conf('postgresql.conf', "shared_buffers='1 GB'");
$replica->start;

sub wait_catchup
{
	my ($primary, $replica) = @_;
	
	my $primary_lsn =
	  $primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
	my $caughtup_query =
	  "SELECT '$primary_lsn'::pg_lsn <= pg_last_wal_replay_lsn()";
	$replica->poll_query_until('postgres', $caughtup_query)
	  or die "Timed out while waiting for standby to catch up";
}

sub repeat_and_time_sql
{
  	my ($name, $node, $sql) = @_;

	my $session =  $node->background_psql('postgres', on_error_die => 1);
	$session->query_safe("SET max_parallel_workers_per_gather=0");

	my $iterations = 0;

	my $now;
	my $elapsed;
    my $begin_time = time();
	while (1) {
		$session->query_safe($sql);
		$now = time();
		$iterations = $iterations + 1;

		$elapsed = $now - $begin_time;
		if ($elapsed > $duration && $iterations >= $miniterations) {
			last;
		}
	}

	my $periter = $elapsed / $iterations;

	pass ("TEST $name: $elapsed s, $iterations iterations, $periter s / iteration");
}


$primary->safe_psql('postgres', "CREATE TABLE little (i int);");
$primary->safe_psql('postgres', "INSERT INTO little VALUES (1);");

sub consume_xids
{
	my ($node) = @_;

	my $session = $node->background_psql('postgres', on_error_die => 1);
	for(my $i = 0; $i < 20; $i++) {
		$session->query_safe(q{do $$
  begin
    for i in 1..50 loop
      begin
        DELETE from little;
        perform 1 / 0;
      exception
        when division_by_zero then perform 0 /* do nothing */;
        when others then raise 'fail: %', sqlerrm;
      end;
    end loop;
  end
$$;});
	}
	$session->quit;
}

# TEST few-xacts
#
# Cycle through 4 different top-level XIDs
#
# 1001, 1002, 1003, 1004, 1001, 1002, 1003, 1004, ...
#
if (1)
{
	$primary->safe_psql('postgres', 'CREATE TABLE tbl(i int)');
	$primary->safe_psql('postgres', "INSERT INTO tbl SELECT g FROM generate_series(1, 100000) g;");
	$primary->safe_psql('postgres', "VACUUM FREEZE tbl;");

	my @primary_sessions = ();
	my $num_connections = 4;
	for(my $i = 0; $i < $num_connections; $i++) {
		my $primary_session =  $primary->background_psql('postgres', on_error_die => 1);
		$primary_session->query_safe("BEGIN;");
		$primary_session->query_safe("DELETE FROM tbl WHERE i % $num_connections = $i;");
		push(@primary_sessions, $primary_session);
	}

	# Consume one more XID, to bump up "last committed XID"
	$primary->safe_psql('postgres', "select txid_current()");

	wait_catchup($primary, $replica);

	repeat_and_time_sql("few-xacts", $replica, "select count(*) from tbl");

	for(my $i = 0; $i < $num_connections; $i++) {
		$primary_sessions[$i]->quit;
	}
	$primary->safe_psql('postgres', "DROP TABLE tbl");
}

# TEST many-xacts
#
# like few-xacts, but we cycle through 100 different XIDs instead of 4.
#
# 1001, 1002, 1003, ... 1100, 1001, 1002, 1003, ... 1100  ....
#
if (1)
{
	$primary->safe_psql('postgres', 'CREATE TABLE tbl(i int)');
	$primary->safe_psql('postgres', "INSERT INTO tbl SELECT g FROM generate_series(1, 100000) g;");
	$primary->safe_psql('postgres', "VACUUM FREEZE tbl;");

	my @primary_sessions = ();
	my $num_connections = 100;
	for(my $i = 0; $i < $num_connections; $i++) {
		my $primary_session =  $primary->background_psql('postgres', on_error_die => 1);
		$primary_session->query_safe("BEGIN;");
		$primary_session->query_safe("DELETE FROM tbl WHERE i % $num_connections = $i;");
		push(@primary_sessions, $primary_session);
	}

	# Consume one more XID, to bump up "last committed XID"
	$primary->safe_psql('postgres', "select txid_current()");

	wait_catchup($primary, $replica);

	repeat_and_time_sql("many-xacts", $replica, "select count(*) from tbl");

	for(my $i = 0; $i < $num_connections; $i++) {
		$primary_sessions[$i]->quit;
	}
	$primary->safe_psql('postgres', "DROP TABLE tbl");
}

# TEST many-xacts-wide-apart
#
# like many-xacts, but the XIDs are more spread out, so that they don't fit in the
# SLRU caches.
#
# 1000, 2000, 3000, 4000, ....
if (1)
{
	$primary->safe_psql('postgres', 'CREATE TABLE tbl(i int)');
	$primary->safe_psql('postgres', "INSERT INTO tbl SELECT g FROM generate_series(1, 100000) g;");
	$primary->safe_psql('postgres', "VACUUM FREEZE tbl;");

	my @primary_sessions = ();
	my $num_connections = 100;
	for(my $i = 0; $i < $num_connections; $i++) {
		my $primary_session =  $primary->background_psql('postgres', on_error_die => 1);

		consume_xids($primary);

		$primary_session->query_safe("BEGIN;");
		$primary_session->query_safe("DELETE FROM tbl WHERE i % $num_connections = $i;");
		push(@primary_sessions, $primary_session);
	}

	# Consume one more XID, to bump up "last committed XID"
	$primary->safe_psql('postgres', "select txid_current()");

	wait_catchup($primary, $replica);

	repeat_and_time_sql("many-xacts-wide-apart", $replica, "select count(*) from tbl");

	for(my $i = 0; $i < $num_connections; $i++) {
		$primary_sessions[$i]->quit;
	}
	$primary->safe_psql('postgres', "DROP TABLE tbl");
}

# TEST: few-subxacts
if (1)
{
	$primary->safe_psql('postgres', 'CREATE TABLE tbl(i int)');
	$primary->safe_psql('postgres', "INSERT INTO tbl SELECT g FROM generate_series(1, 100000) g;");
	$primary->safe_psql('postgres', "VACUUM FREEZE tbl;");

	my $primary_session =  $primary->background_psql('postgres', on_error_die => 1);
	$primary_session->query_safe("BEGIN;");
	my $num_subxacts = 4;
	for(my $i = 0; $i < $num_subxacts; $i++) {
		$primary_session->query_safe("savepoint sp$i;");
		$primary_session->query_safe("DELETE FROM tbl WHERE i % $num_subxacts = $i;");
		$primary_session->query_safe("release savepoint sp$i;");
	}

	# Consume one more XID, to bump up "last committed XID"
	$primary->safe_psql('postgres', "select txid_current()");

	wait_catchup($primary, $replica);

	repeat_and_time_sql("few-subxacts", $replica, "select count(*) from tbl");

	$primary_session->quit;
	$primary->safe_psql('postgres', "DROP TABLE tbl");
}


# TEST: many-subxacts
if (1)
{
	$primary->safe_psql('postgres', 'CREATE TABLE tbl(i int)');
	$primary->safe_psql('postgres', "INSERT INTO tbl SELECT g FROM generate_series(1, 100000) g;");
	$primary->safe_psql('postgres', "VACUUM FREEZE tbl;");

	my $primary_session =  $primary->background_psql('postgres', on_error_die => 1);
	$primary_session->query_safe("BEGIN;");
	my $num_subxacts = 1000;
	for(my $i = 0; $i < $num_subxacts; $i++) {
		$primary_session->query_safe("savepoint sp$i;");
		$primary_session->query_safe("DELETE FROM tbl WHERE i % $num_subxacts = $i;");
		$primary_session->query_safe("release savepoint sp$i;");
	}

	# Consume one more XID, to bump up "last committed XID"
	$primary->safe_psql('postgres', "select txid_current()");

	wait_catchup($primary, $replica);

	repeat_and_time_sql("many-subxacts", $replica, "select count(*) from tbl");

	$primary_session->quit;
	$primary->safe_psql('postgres', "DROP TABLE tbl");
}

# TEST: many-subxacts-wide-apart
if (1)
{
	$primary->safe_psql('postgres', 'CREATE TABLE tbl(i int)');
	$primary->safe_psql('postgres', "INSERT INTO tbl SELECT g FROM generate_series(1, 100000) g;");
	$primary->safe_psql('postgres', "VACUUM FREEZE tbl;");

	my $primary_session =  $primary->background_psql('postgres', on_error_die => 1);
	$primary_session->query_safe("BEGIN;");
	my $num_subxacts = 1000;
	for(my $i = 0; $i < $num_subxacts; $i++) {
		consume_xids($primary);
		$primary_session->query_safe("savepoint sp$i;");
		$primary_session->query_safe("DELETE FROM tbl WHERE i % $num_subxacts = $i;");
		$primary_session->query_safe("release savepoint sp$i;");
	}

	# Consume one more XID, to bump up "last committed XID"
	$primary->safe_psql('postgres', "select txid_current()");

	wait_catchup($primary, $replica);

	repeat_and_time_sql("many-subxacts-wide-apart", $replica, "select count(*) from tbl");

	$primary_session->quit;
	$primary->safe_psql('postgres', "DROP TABLE tbl");
}

# TEST: insert-all-different-xids
if (1)
{
	$primary->safe_psql('postgres', 'CREATE TABLE tbl(i int)');

	my @primary_sessions = ();
	my $num_connections = 1000;
	for(my $i = 0; $i < $num_connections; $i++) {
		my $primary_session =  $primary->background_psql('postgres', on_error_die => 1);
		$primary_session->query_safe("BEGIN;");
		$primary_session->query_safe("INSERT INTO tbl VALUES ($i)");
		push(@primary_sessions, $primary_session);
	}

	# Consume one more XID, to bump up "last committed XID"
	$primary->safe_psql('postgres', "select txid_current()");

	wait_catchup($primary, $replica);

	repeat_and_time_sql("insert-all-different-xids", $replica, "select count(*) from tbl");

	for(my $i = 0; $i < $num_connections; $i++) {
		$primary_sessions[$i]->quit;
	}
	$primary->safe_psql('postgres', "DROP TABLE tbl");
}

# TEST: insert-all-different-subxids
if (1)
{
	$primary->safe_psql('postgres', 'CREATE TABLE tbl(i int)');
	$primary->safe_psql('postgres', "VACUUM FREEZE tbl;");

	my $primary_session =  $primary->background_psql('postgres', on_error_die => 1);
	$primary_session->query_safe("BEGIN;");
	my $num_subxacts = 1000;
	for(my $i = 0; $i < $num_subxacts; $i++) {
		$primary_session->query_safe("savepoint sp$i; INSERT INTO tbl VALUES($i); release savepoint sp$i;");
	}

	# Consume one more XID, to bump up "last committed XID"
	$primary->safe_psql('postgres', "select txid_current()");

	wait_catchup($primary, $replica);

	repeat_and_time_sql("insert-all-different-subxids", $replica, "select count(*) from tbl");

	$primary_session->quit;
	$primary->safe_psql('postgres', "DROP TABLE tbl");
}

done_testing();
