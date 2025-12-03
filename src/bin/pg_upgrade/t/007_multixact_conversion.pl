# Copyright (c) 2025, PostgreSQL Global Development Group

# Version 19 expanded MultiXactOffset from 32 to 64 bits. Upgrading
# across that requires rewriting the SLRU files to the new format.
# This file contains tests for the conversion.
#
# To run, set 'oldinstall' ENV variable to point to a pre-v19
# installation. If it's not set, or if it points to a v19 or above
# installation, this still performs a very basic test, upgrading a
# cluster with some multixacts. It's not very interesting, however,
# because there's no conversion involved in that case.

use strict;
use warnings FATAL => 'all';

use Math::BigInt;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Temp dir for a dumps.
my $tempdir = PostgreSQL::Test::Utils::tempdir;

# A workload that consumes multixids. The purpose of this is to
# generate some multixids in the old cluster, so that we can test
# upgrading them. The workload is a mix of KEY SHARE locking queries
# and UPDATEs, and commits and aborts, to generate a mix of multixids
# with different statuses. It consumes around 3000 multixids with
# 30000 members. That's enough to span more than one multixids
# 'offsets' page, and more than one 'members' segment.
#
# The workload leaves behind a table called 'mxofftest' containing a
# small number of rows referencing some of the generated multixids.
#
# Because this function is used to generate test data on the old
# installation, it needs to work with older PostgreSQL server
# versions.
#
# The first argument is the cluster to connect to, the second argument
# is a cluster using the new version. We need the 'psql' binary from
# the new version, the new cluster is otherwise unused. (We need to
# use the new 'psql' because some of the more advanced background psql
# perl module features depend on a fairly recent psql version.)
sub mxact_workload
{
	my $node = shift;       # Cluster to connect to
	my $binnode = shift;    # Use the psql binary from this cluster

	my $connstr = $node->connstr('postgres');

	$node->start;
	$node->safe_psql('postgres', qq[
		CREATE TABLE mxofftest (id INT PRIMARY KEY, n_updated INT)
		  WITH (AUTOVACUUM_ENABLED=FALSE);
		INSERT INTO mxofftest SELECT G, 0 FROM GENERATE_SERIES(1, 50) G;
	]);

	my $nclients = 20;
	my $update_every = 13;
	my $abort_every = 11;
	my @connections = ();

	# Silence the logging of the statements we run to avoid
	# unnecessarily bloating the test logs. This runs before the
	# upgrade we're testing, so the details should not be very
	# interesting for debugging. But if needed, you can make it more
	# verbose by setting this.
	my $verbose = 0;

	# Open multiple connections to the database. Start a transaction
	# in each connection.
	for (0 .. $nclients)
	{
		# Use the psql binary from the new installation. The
		# BackgroundPsql functionality doesn't work with older psql
		# versions.
		my $conn = $binnode->background_psql('',
			connstr => $node->connstr('postgres'));

		$conn->query_safe("SET log_statement=none", verbose => $verbose) unless $verbose;
		$conn->query_safe("SET enable_seqscan=off", verbose => $verbose);
		$conn->query_safe("BEGIN", verbose => $verbose);

		push(@connections, $conn);
	}

	# Run queries using cycling through the connections in a
	# round-robin fashion. We keep a transaction open in each
	# connection at all times, and lock/update the rows. With 10
	# connections, each SELECT FOR KEY SHARE query generates a new
	# multixid, containing the 10 XIDs of all the transactions running
	# at the time.
	for (my $i = 0; $i < 3000; $i++)
	{
		my $conn = $connections[ $i % $nclients ];

		my $sql;
		if ($i % $abort_every == 0)
		{
			$sql = "ABORT; ";
		}
		else
		{
			$sql = "COMMIT; ";
		}
		$sql .= "BEGIN; ";

		if ($i % $update_every == 0)
		{
			$sql .= qq[
			  UPDATE mxofftest SET n_updated = n_updated + 1 WHERE id = ${i} % 50;
			];
		}
		else
		{
			my $threshold = int($i / 3000 * 50);
			$sql .= qq[
			  select count(*) from (
				SELECT * FROM mxofftest WHERE id >= $threshold FOR KEY SHARE
			  ) as x
			];
		}
		$conn->query_safe($sql, verbose => $verbose);
	}

	for my $conn (@connections)
	{
		$conn->quit();
	}

	$node->stop;
	return;
}

# Return contents of the 'mxofftest' table, created by mxact_workload
sub get_test_table_contents
{
	my ($node, $file_prefix) = @_;

	my $contents = $node->safe_psql('postgres',
		"SELECT ctid, xmin, xmax, * FROM mxofftest");

	my $dumpfile = $tempdir . '/' . $file_prefix . '.sql';
	open(my $dh, '>', $dumpfile)
	  || die "could not open $dumpfile for writing $!";
	print $dh $contents;
	close($dh);

	return $dumpfile;
}

# Read NextMultiOffset from the control file
#
# Note: This is used on both the old and the new installation, so the
# command arguments and the output parsing used here must work with
# all PostgreSQL versions supported by the test.
sub read_next_mxoff
{
	my $node = shift;

	my $pg_controldata_path = $node->installed_command('pg_controldata');
	my ($stdout, $stderr) =
	  run_command([ $pg_controldata_path, $node->data_dir ]);
	$stdout =~ /^Latest checkpoint's NextMultiOffset:\s*(.*)$/m
	  or die "could not read NextMultiOffset from pg_controldata";
	return $1;
}

# Reset a cluster's oldest multixact-offset to given offset.
#
# Note: This is used on both the old and the new installation, so the
# command arguments and the output parsing used here must work with
# all PostgreSQL versions supported by the test.
sub reset_mxoff_pre_v19
{
	my $node = shift;
	my $offset = shift;

	my $pg_resetwal_path = $node->installed_command('pg_resetwal');
	# Get block size
	my ($out, $err) =
	  run_command([ $pg_resetwal_path, '--dry-run', $node->data_dir ]);
	$out =~ /^Database block size: *(\d+)$/m or die;
	my $blcksz = $1;
	# SLRU_PAGES_PER_SEGMENT is always 32 on pre-19 version
	my $slru_pages_per_segment = 32;

	# Verify that no multixids are currently in use. Resetting would
	# destroy them. (A freshly initialized cluster has no multixids.)
	$out =~ /^Latest checkpoint's NextMultiXactId: *(\d+)$/m or die;
	my $next_mxid = $1;
	$out =~ /^Latest checkpoint's oldestMultiXid: *(\d+)$/m or die;
	my $oldest_mxid = $1;
	die "cluster has some multixids in use" unless $next_mxid == $oldest_mxid;

	# Reset to new offset using pg_resetwal
	my @cmd = (
		$pg_resetwal_path,
		'--pgdata' => $node->data_dir,
		'--multixact-offset' => $offset);
	command_ok(\@cmd, 'set oldest multixact-offset');

	# pg_resetwal just updates the control file. The cluster will
	# refuse to start up, if the SLRU segment corresponding to the
	# offset does not exist. Create a dummy segment that covers the
	# given offset, filled with zeros. But first remove any old
	# segments.
	unlink glob $node->data_dir . "/pg_multixact/members/*";

	my $mult = 32 * int($blcksz / 20) * 4;
	my $segname = sprintf "%04X", $offset / $mult;

	my $path = $node->data_dir . "/pg_multixact/members/" . $segname;

	my $null_block = "\x00" x $blcksz;
	open(my $dh, '>', $path)
	  || die "could not open $path for writing $!";
	for (0 .. $slru_pages_per_segment)
	{
		print $dh $null_block;
	}
	close($dh);
}

# Main test workhorse routine.
# Dump data on old version, run pg_upgrade, compare data after upgrade.
sub upgrade_and_compare
{
	my $tag = shift;
	my $oldnode = shift;
	my $newnode = shift;

	command_ok(
		[
			'pg_upgrade', '--no-sync',
			'--old-datadir' => $oldnode->data_dir,
			'--new-datadir' => $newnode->data_dir,
			'--old-bindir' => $oldnode->config_data('--bindir'),
			'--new-bindir' => $newnode->config_data('--bindir'),
			'--socketdir' => $newnode->host,
			'--old-port' => $oldnode->port,
			'--new-port' => $newnode->port,
		],
		'run of pg_upgrade for new instance');

	# Note: we do this *after* running pg_upgrade, to ensure that we
	# don't set all the hint bits before upgrade by doing the SELECT
	# on the table.
	$oldnode->start;
	my $old_dump = get_test_table_contents($oldnode, "oldnode_${tag}_dump");
	$oldnode->stop;

	$newnode->start;
	my $new_dump = get_test_table_contents($newnode, "newnode_${tag}_dump");
	$newnode->stop;

	compare_files($old_dump, $new_dump,
		'test table contents from original and upgraded databases match');
}

my $old_version;

# Basic scenario: Create a cluster using old installation, run
# multixid-creating workload on it, then upgrade.
#
# This works even even if the old and new version is the same,
# although it's not very interesting as the conversion routines only
# run when upgrading from a pre-v19 cluster.
{
	my $tag = 'basic';
	my $old =
	  PostgreSQL::Test::Cluster->new("${tag}_oldnode",
		install_path => $ENV{oldinstall});
	my $new = PostgreSQL::Test::Cluster->new("${tag}_newnode");

	$old->init(extra => ['-k']);

	$old_version = $old->pg_version;
	note "old installation is version $old_version\n";

	# Run the workload
	my $start_mxoff = read_next_mxoff($old);
	mxact_workload($old, $new);
	my $finish_mxoff = read_next_mxoff($old);

	$new->init;
	upgrade_and_compare($tag, $old, $new);

	my $new_next_mxoff = read_next_mxoff($new);

	note ">>> case #${tag}\n"
	  . " oldnode mxoff from ${start_mxoff} to ${finish_mxoff}\n"
	  . " newnode mxoff ${new_next_mxoff}\n";
}

# Wraparound scenario: This is the same as the basic scenario, but the
# old cluster goes through mxoffset wraparound.
#
# This requires the old installation to be version 19 of older,
# because the hacks we use to reset the old cluster to a state just
# before the wraparound rely on the pre-v19 file format. In version
# 19, offsets no longer wrap around anyway.
SKIP:
{
	skip
	  "skipping mxoffset conversion tests because upgrading from the old version does not require conversion"
	  if ($old_version >= '19devel');

	my $tag = 'wraparound';
	my $old =
	  PostgreSQL::Test::Cluster->new("${tag}_oldnode",
		install_path => $ENV{oldinstall});
	my $new = PostgreSQL::Test::Cluster->new("${tag}_newnode");

	$old->init(extra => ['-k']);

	# Reset the NextMultiOffset value in the  old cluster to just before 32-bit wraparound.
	reset_mxoff_pre_v19($old, 0xFFFFEC77);

	# Run the workload. This crosses the wraparound.
	my $start_mxoff = read_next_mxoff($old);
	mxact_workload($old, $new);
	my $finish_mxoff = read_next_mxoff($old);

	# Verify that wraparound happened.
	cmp_ok($finish_mxoff, '<', $start_mxoff,
		"mxoff wrapped around in old cluster");

	$new->init;
	upgrade_and_compare($tag, $old, $new);

	my $new_next_mxoff = read_next_mxoff($new);

	note ">>> case #${tag}\n"
	  . " oldnode mxoff from ${start_mxoff} to ${finish_mxoff}\n"
	  . " newnode mxoff ${new_next_mxoff}\n";
}

done_testing();
