
# Copyright (c) 2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use Math::BigInt;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::AdjustDump;
use PostgreSQL::Test::AdjustUpgrade;
use Test::More;

# This test involves different multitransaction states, similarly to that of
# 002_pg_upgrade.pl.

unless (defined($ENV{oldinstall}))
{
	plan skip_all =>
		'to run test set oldinstall environment variable to the pre 64-bit mxoff cluster';
}

# Temp dir for a dumps.
my $tempdir = PostgreSQL::Test::Utils::tempdir;

# Can be changed to test the other modes.
my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

sub utility_path
{
	my $node = shift;
	my $name = shift;

	my $bin_path = defined($node->install_path) ?
		$node->install_path . "/bin/$name" : $name;

	return $bin_path;
}

# Get NextMultiOffset.
sub next_mxoff
{
	my $node = shift;

	my $pg_controldata_path = utility_path($node, 'pg_controldata');
	my ($stdout, $stderr) = run_command([ $pg_controldata_path,
											$node->data_dir ]);
	my @control_data = split("\n", $stdout);
	my $next_mxoff = undef;

	foreach (@control_data)
	{
		if ($_ =~ /^Latest checkpoint's NextMultiOffset:\s*(.*)$/mg)
		{
			$next_mxoff = $1;
			last;
		}
	}
	die "NextMultiOffset not found in control file\n"
		unless defined($next_mxoff);

	return $next_mxoff;
}

# Consume around 10k of mxoffsets.
sub mxact_eater
{
	my $node = shift;
	my $tbl = 'FOO';

	my ($mxoff1, $mxoff2);

	$mxoff1 = next_mxoff($node);
	$node->start;
	$node->safe_psql('postgres',
		"CREATE TABLE ${tbl} (I INT PRIMARY KEY, N_UPDATED INT) " .
		"       WITH (AUTOVACUUM_ENABLED=FALSE);" .
		"INSERT INTO ${tbl} SELECT G, 0 FROM GENERATE_SERIES(1, 50) G;");

	# consume around 10k mxoff
	my $nclients = 10;
	my $update_every = 75;
	my @connections = ();

	for (0..$nclients)
	{
		my $conn = $node->background_psql('postgres');
		$conn->query_safe("BEGIN");

		push(@connections, $conn);
	}

	for (my $i = 0; $i < 1000; $i++)
	{
		my $conn = $connections[$i % $nclients];

		$conn->query_safe("COMMIT;");
		$conn->query_safe("BEGIN");

		if ($i % $update_every == 0)
		{
			$conn->query_safe(
				"UPDATE ${tbl} SET " .
				"N_UPDATED = N_UPDATED + 1 " .
				"WHERE I = ${i} % 50");
		}
		else
		{
			$conn->query_safe(
				"SELECT * FROM ${tbl} FOR KEY SHARE");
		}
	}

	for my $conn (@connections)
	{
		$conn->quit();
	}

	$node->stop;
	$mxoff2 = next_mxoff($node);

	return $mxoff1, $mxoff2;
}

# Consume around 1M of mxoffsets.
sub mxact_huge_eater
{
	my $node = shift;
	my $tbl = 'FOO';

	my ($mxoff1, $mxoff2);

	$mxoff1 = next_mxoff($node);
	$node->start;
	$node->safe_psql('postgres',
		"CREATE TABLE ${tbl} (I INT PRIMARY KEY, N_UPDATED INT) " .
		"       WITH (AUTOVACUUM_ENABLED=FALSE);" .
		"INSERT INTO ${tbl} SELECT G, 0 FROM GENERATE_SERIES(1, 4) G;");

	my $nclients = 100;
	my @connections = ();
	my $timeout = 10 * $PostgreSQL::Test::Utils::timeout_default;

	for (0..$nclients)
	{
		my $conn = $node->background_psql('postgres',
										  timeout => $timeout);
		$conn->query_safe("BEGIN");

		push(@connections, $conn);
	}

	# It's a long process, better to tell about progress.
	my $n_steps = 100_000;
	my $step = int($n_steps / 10);

	diag "\nstart to consume mxoffsets ...\n";
	for (my $i = 0; $i < $n_steps; $i++)
	{
		my $conn = $connections[$i % $nclients];

		$conn->query_safe("COMMIT;");
		$conn->query_safe("BEGIN");

		{
			$conn->query_safe(
				"SELECT * FROM ${tbl} " .
				"FOR KEY SHARE");
		}

		if ($i % $step == 0)
		{
			my $done = int(($i / $n_steps) * 100);
			diag "$done% done...";
		}
	}

	for my $conn (@connections)
	{
		$conn->quit();
	}

	$node->stop;
	$mxoff2 = next_mxoff($node);

	return $mxoff1, $mxoff2;
}

# Set oldest multixact-offset
sub reset_mxoff
{
	my $node = shift;
	my $offset = shift;

	my $pg_resetwal_path = utility_path($node, 'pg_resetwal');
	# Get block size
	my $out = (run_command([ $pg_resetwal_path, '--dry-run',
							 $node->data_dir ]))[0];
		$out =~ /^Database block size: *(\d+)$/m or die;
	my $blcksz = $1;

	# Reset to new offset
	my @cmd = ($pg_resetwal_path, '--pgdata' => $node->data_dir);
	push @cmd, '--multixact-offset' => $offset;
	command_ok(\@cmd, 'set oldest multixact-offset');

	# Fill empty pg_multixact/members segment
	my $mult = 32 * int($blcksz / 20) * 4;
	my $segname = sprintf "%04X", $offset / $mult;

	my @dd = ('dd');
	push @dd, "if=/dev/zero";
	push @dd, "of=" . $node->data_dir . "/pg_multixact/members/" . $segname;
	push @dd, "bs=$blcksz";
	push @dd, "count=32";
	command_ok(\@dd, 'fill empty multixact-members');
}

sub get_dump_for_comparison
{
	my ($node, $db, $file_prefix, $adjust_child_columns) = @_;

	my $dumpfile = $tempdir . '/' . $file_prefix . '.sql';
	my $dump_adjusted = "${dumpfile}_adjusted";

	open(my $dh, '>', $dump_adjusted)
	  || die "could not open $dump_adjusted for writing $!";

	my $pg_dump_path = utility_path($node, 'pg_dump');

	$node->run_log(
		[
			$pg_dump_path, '--no-sync',
			'--restrict-key' => 'test',
			'-d' => $node->connstr($db),
			'-f' => $dumpfile
		]);

	print $dh adjust_regress_dumpfile(slurp_file($dumpfile),
		$adjust_child_columns);
	close($dh);

	return $dump_adjusted;
}

# Main test workhorse routine.
# Make pg_upgrade, dump data and compare it.
sub run_test
{
	my $tag = shift;
	my $oldnode = shift;
	my $newnode = shift;

	my $pg_upgrade_path = utility_path($newnode, 'pg_upgrade');

	command_ok(
		[
			$pg_upgrade_path, '--no-sync',
			'--old-datadir' => $oldnode->data_dir,
			'--new-datadir' => $newnode->data_dir,
			'--old-bindir' => $oldnode->config_data('--bindir'),
			'--new-bindir' => $newnode->config_data('--bindir'),
			'--socketdir' => $newnode->host,
			'--old-port' => $oldnode->port,
			'--new-port' => $newnode->port,
			$mode,
		],
		'run of pg_upgrade for new instance');
	ok( !-d $newnode->data_dir . "/pg_upgrade_output.d",
		"pg_upgrade_output.d/ removed after pg_upgrade success");

	$oldnode->start;
	my $src_dump =
		get_dump_for_comparison($oldnode, 'postgres',
								"oldnode_${tag}_dump", 0);
	$oldnode->stop;

	$newnode->start;
	my $dst_dump =
		get_dump_for_comparison($newnode, 'postgres',
								"newnode_${tag}_dump", 0);
	$newnode->stop;

	compare_files($src_dump, $dst_dump,
		'dump outputs from original and restored regression databases match');
}

sub to_hex
{
	my $arg = shift;

	$arg = Math::BigInt->new($arg);
	$arg = $arg->as_hex();

	return $arg;
}

# case #1: start old node from defaults
{
	my $tag = 1;
	my $old =
		PostgreSQL::Test::Cluster->new("oldnode${tag}",
									   install_path => $ENV{oldinstall});
	$old->init(extra => ['-k']);

	my ($start_mxoff, $finish_mxoff) = mxact_eater($old);

	my $new = PostgreSQL::Test::Cluster->new("newnode${tag}");
	$new->init;

	run_test($tag, $old, $new);

	$start_mxoff = to_hex($start_mxoff);
	$finish_mxoff = to_hex($finish_mxoff);

	my $next_mxoff = to_hex(next_mxoff($new));

	note ">>> case #${tag}\n" .
		 " oldnode mxoff from ${start_mxoff} to ${finish_mxoff}\n" .
		 " newnode mxoff ${next_mxoff}\n";
}

# case #2: start old node from before 32-bit wraparound
{
	my $tag = 2;
	my $old =
		PostgreSQL::Test::Cluster->new("oldnode${tag}",
									   install_path => $ENV{oldinstall});

	$old->init(extra => ['-k']);
	reset_mxoff($old, 0xFFFF0000);

	my ($start_mxoff, $finish_mxoff) = mxact_eater($old);

	my $new = PostgreSQL::Test::Cluster->new("newnode${tag}");
	$new->init;

	run_test($tag, $old, $new);

	$start_mxoff = to_hex($start_mxoff);
	$finish_mxoff = to_hex($finish_mxoff);

	my $next_mxoff = to_hex(next_mxoff($new));

	note ">>> case #${tag}\n" .
		 " oldnode mxoff from ${start_mxoff} to ${finish_mxoff}\n" .
		 " newnode mxoff ${next_mxoff}\n";
}

# case #3: start old node near 32-bit wraparound and reach wraparound state.
{
	my $tag = 3;
	my $old =
		PostgreSQL::Test::Cluster->new("oldnode${tag}",
									   install_path => $ENV{oldinstall});

	$old->init(extra => ['-k']);

	reset_mxoff($old, 0xFFFFEC77);
	my ($start_mxoff, $finish_mxoff) = mxact_eater($old);

	my $new = PostgreSQL::Test::Cluster->new("newnode${tag}");
	$new->init;

	run_test($tag, $old, $new);

	$start_mxoff = to_hex($start_mxoff);
	$finish_mxoff = to_hex($finish_mxoff);

	my $next_mxoff = to_hex(next_mxoff($new));

	note ">>> case #${tag}\n" .
		 " oldnode mxoff from ${start_mxoff} to ${finish_mxoff}\n" .
		 " newnode mxoff ${next_mxoff}\n";
}

# case #4: start old node from defaults
{
	my $tag = 4;
	my $old =
		PostgreSQL::Test::Cluster->new("oldnode${tag}",
									   install_path => $ENV{oldinstall});

	$old->init(extra => ['-k']);
	$old->append_conf("postgresql.conf", "max_connections = 128");

	diag "test #${tag} for multiple mxoff segments";
	my ($start_mxoff, $finish_mxoff) = mxact_huge_eater($old);

	my $new = PostgreSQL::Test::Cluster->new("newnode${tag}");
	$new->init;

	run_test($tag, $old, $new);

	$start_mxoff = to_hex($start_mxoff);
	$finish_mxoff = to_hex($finish_mxoff);

	my $next_mxoff = to_hex(next_mxoff($new));

	note ">>> case #${tag}\n" .
		 " oldnode mxoff from ${start_mxoff} to ${finish_mxoff}\n" .
		 " newnode mxoff ${next_mxoff}\n";
}

# case #5: start old node from before 32-bit wraparound
{
	my $tag = 5;
	my $old =
		PostgreSQL::Test::Cluster->new("oldnode${tag}",
									   install_path => $ENV{oldinstall});

	$old->init(extra => ['-k']);
	$old->append_conf("postgresql.conf", "max_connections = 128");
	reset_mxoff($old, 0xFF000000);

	diag "test #${tag} for multiple mxoff segments";
	my ($start_mxoff, $finish_mxoff) = mxact_huge_eater($old);

	my $new = PostgreSQL::Test::Cluster->new("newnode${tag}");
	$new->init;

	run_test($tag, $old, $new);

	$start_mxoff = to_hex($start_mxoff);
	$finish_mxoff = to_hex($finish_mxoff);

	my $next_mxoff = to_hex(next_mxoff($new));

	note ">>> case #${tag}\n" .
		 " oldnode mxoff from ${start_mxoff} to ${finish_mxoff}\n" .
		 " newnode mxoff ${next_mxoff}\n";
}

# case #6: start old node near 32-bit wraparound and reach wraparound state.
{
	my $tag = 6;
	my $old =
		PostgreSQL::Test::Cluster->new("oldnode${tag}",
									   install_path => $ENV{oldinstall});

	$old->init(extra => ['-k']);

	reset_mxoff($old, 0xFFFFFFFF - 500_000);
	$old->append_conf("postgresql.conf", "max_connections = 128");
	my ($start_mxoff, $finish_mxoff) = mxact_huge_eater($old);

	diag "test #${tag} for multiple mxoff segments";
	my $new = PostgreSQL::Test::Cluster->new("newnode${tag}");
	$new->init;

	run_test($tag, $old, $new);

	$start_mxoff = to_hex($start_mxoff);
	$finish_mxoff = to_hex($finish_mxoff);

	my $next_mxoff = to_hex(next_mxoff($new));

	note ">>> case #${tag}\n" .
		 " oldnode mxoff from ${start_mxoff} to ${finish_mxoff}\n" .
		 " newnode mxoff ${next_mxoff}\n";
}

done_testing();
