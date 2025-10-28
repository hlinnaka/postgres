
# Copyright (c) 2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use Math::BigInt;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub mxact_eater
{
	my $node = shift;
	my $tbl = shift;

	$node->start;
	$node->safe_psql('postgres',
		"CREATE TABLE ${tbl} (I INT PRIMARY KEY, N_UPDATED INT) " .
		"       WITH (AUTOVACUUM_ENABLED=FALSE);" .
		"INSERT INTO ${tbl} SELECT G, 0 FROM GENERATE_SERIES(1, 50) G;");

	# consume around 10k multixact-offsetfs
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
}

sub next_mxoff
{
	my $node = shift;
	my ($stdout, $stderr) =
	  run_command([ 'pg_controldata', $node->data_dir ]);
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

sub reset_mxoff
{
	my $node = shift;
	my $offset = shift;
		$offset = Math::BigInt->new($offset);

	# Get block size
	my $out = (run_command([ 'pg_resetwal', '--dry-run', $node->data_dir ]))[0];
		$out =~ /^Database block size: *(\d+)$/m or die;
	my $blcksz = $1;

	# Reset to new offset
	my @cmd = ('pg_resetwal', '--pgdata' => $node->data_dir);
	push @cmd, '--multixact-offset' => $offset->as_hex();
	command_ok(\@cmd, 'set oldest multixact-offset');

	# Fill empty pg_multixact/members segment
	my $mult = 32 * int($blcksz / 20) * 4;
	my $segname = sprintf "%015X", $offset / $mult;

	my @dd = ('dd');
	push @dd, "if=/dev/zero";
	push @dd, "of=" . $node->data_dir . "/pg_multixact/members/" . $segname;
	push @dd, "bs=$blcksz";
	push @dd, "count=32";
	command_ok(\@dd, 'fill empty multixact-members');
}

my ($off1, $off2);

# start from defaults
my $node1 = PostgreSQL::Test::Cluster->new('node1');
$node1->init;
$off1 = next_mxoff($node1);
mxact_eater($node1, "FOO");
$off2 = next_mxoff($node1);
note "> start from $off1, finished at $off2\n";

# start from before 32-bit wraparound
my $node2 = PostgreSQL::Test::Cluster->new('node2');
$node2->init;
reset_mxoff($node2, 0xFFFF0000);
$off1 = next_mxoff($node2);
mxact_eater($node2, "FOO");
$off2 = next_mxoff($node2);
note "> start from $off1, finished at $off2\n";

# start near 32-bit wraparound
my $node3 = PostgreSQL::Test::Cluster->new('node3');
$node3->init;
reset_mxoff($node3, 0xFFFFEC77);
$off1 = next_mxoff($node3);
mxact_eater($node3, "FOO");
$off2 = next_mxoff($node3);
note "> start from $off1, finished at $off2\n";

# start over 32-bit wraparound
my $node4 = PostgreSQL::Test::Cluster->new('node4');
$node4->init;
reset_mxoff($node4, '0xFFFFFFFF0000');
$off1 = next_mxoff($node4);
mxact_eater($node4, "FOO");
$off2 = next_mxoff($node3);
note "> start from $off1, finished at $off2\n";

# check invariant
$node1->start;
$node2->start;
$node3->start;
$node4->start;

my $var1 = $node1->safe_psql('postgres', 'TABLE FOO');
my $var2 = $node2->safe_psql('postgres', 'TABLE FOO');
my $var3 = $node3->safe_psql('postgres', 'TABLE FOO');
my $var4 = $node4->safe_psql('postgres', 'TABLE FOO');
ok($var1 eq $var2 eq $var3 eq $var4,
	'check table invariant in all nodes');

$node4->stop;
$node3->stop;
$node2->stop;
$node1->stop;

done_testing();
