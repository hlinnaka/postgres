
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Verify that ALTER TABLE optimizes certain operations as expected

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);

# Initialize a test cluster
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init();
# Turn message level up to DEBUG1 so that we get the messages we want to see
$primary->append_conf('postgresql.conf', 'max_wal_senders = 5');
$primary->append_conf('postgresql.conf', 'wal_level=replica');
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

sub repeat_and_time_psql
{
  	my ($name, $node, $repeats, $sql) = @_;

    my $begin_time = time();

	$node->safe_psql('postgres', qq{
set max_parallel_workers_per_gather=0;
do \$\$
 begin
   for i in 1..$repeats loop
     $sql;
   end loop;
end;
\$\$});
	
	my $end_time = time();
	my $elapsed = $end_time - $begin_time;

	pass ("TEST $name: $elapsed");
}

# TEST 1: A transaction open in primary that inserts a lot of rows. SeqScan
# the table on the replica. It sees all the XIDs as not in-progress

$primary->safe_psql('postgres', 'CREATE TABLE tbl(i int)');

my $primary_session =  $primary->background_psql('postgres', on_error_die => 1);
$primary_session->query_safe("BEGIN;");
$primary_session->query_safe("INSERT INTO tbl SELECT g FROM generate_series(1, 100000) g;");

# Consume one more XID, to bump up "last committed XID"
$primary->safe_psql('postgres', "select txid_current()");

wait_catchup($primary, $replica);

repeat_and_time_psql("large-xact", $replica, 5000, "perform * from tbl");

$primary_session->quit;
$primary->safe_psql('postgres', "DROP TABLE tbl");

# TEST 2: Like 'large-xact', but with lots of subxacts

$primary->safe_psql('postgres', 'CREATE TABLE tbl(i int)');

$primary_session =  $primary->background_psql('postgres', on_error_die => 1);
$primary_session->query_safe("BEGIN;");
$primary_session->query_safe(q{
do $$
  begin
    for i in 1..100000 loop
      begin
        insert into tbl values (i);
      exception
        when others then raise 'fail: %', sqlerrm;
      end;
    end loop;
  end
$$;
});

# Consume one more XID, to bump up "last committed XID"
$primary->safe_psql('postgres', "select txid_current()");

wait_catchup($primary, $replica);

repeat_and_time_psql("many-subxacts", $replica, 1000, "perform * from tbl");

$primary_session->quit;

done_testing();
