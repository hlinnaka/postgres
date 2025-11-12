
# Copyright (c) 2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use Math::BigInt;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::AdjustDump;
use PostgreSQL::Test::AdjustUpgrade;
use Test::More;

# Temp dir for a dumps.
my $tempdir = PostgreSQL::Test::Utils::tempdir;

# Can be changed to test the other modes.
my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

# Handy pg_resetwal wrapper
sub reset_mxoff
{
	my %args = @_;

	my $node = $args{node};
	my $offset = $args{offset};
	my $multi = $args{multi};
	my $blcksz = sub # Get block size
	{
		my $out = (run_command([ 'pg_resetwal', '--dry-run',
								 $node->data_dir ]))[0];
		$out =~ /^Database block size: *(\d+)$/m or die;
		return $1;
	}->();

	my @cmd;

	# Reset cluster
	@cmd = ('pg_resetwal', '--pgdata' => $node->data_dir);
	if (defined($offset))
	{
		push @cmd, '--multixact-offset' => $offset;
	}
	if (defined($multi))
	{
		push @cmd, "--multixact-ids=$multi,$multi";
	}
	command_ok(\@cmd, 'reset multi/offset');

	my $n_items;
	my $segname;

	# Fill empty pg_multixact segments
	if (defined($offset))
	{
		$n_items = 32 * int($blcksz / 20) * 4;
		$segname = sprintf "%015X", ($offset / $n_items);
		$segname = $node->data_dir . "/pg_multixact/members/" . $segname;

		@cmd = ('dd');
		push @cmd, "if=/dev/zero";
		push @cmd, "of=" . $segname;
		push @cmd, "bs=$blcksz";
		push @cmd, "count=32";
		command_ok(\@cmd, 'fill empty multixact-members');
	}

	if (defined($multi))
	{
		$n_items = 32 * int($blcksz / 8);
		$segname = sprintf "%04X", $multi / $n_items;
		$segname = $node->data_dir . "/pg_multixact/offsets/" . $segname;

		@cmd = ('dd');
		push @cmd, "if=/dev/zero";
		push @cmd, "of=" . $segname;
		push @cmd, "bs=$blcksz";
		push @cmd, "count=32";
		command_ok(\@cmd, 'fill empty multixact-offsets');
	}
}

sub get_dump_for_comparison
{
	my ($node, $db, $file_prefix, $adjust_child_columns) = @_;

	my $dumpfile = $tempdir . '/' . $file_prefix . '.sql';
	my $dump_adjusted = "${dumpfile}_adjusted";

	open(my $dh, '>', $dump_adjusted)
	  || die "could not open $dump_adjusted for writing $!";

	$node->run_log(
		[
			'pg_dump', '--no-sync',
			'--restrict-key' => 'test',
			'-d' => $node->connstr($db),
			'-f' => $dumpfile
		]);

	print $dh adjust_regress_dumpfile(slurp_file($dumpfile),
		$adjust_child_columns);
	close($dh);

	return $dump_adjusted;
}

# Create old node
my $old = PostgreSQL::Test::Cluster->new("old");
$old->init;
reset_mxoff(node => $old, multi => 4294967295, offset => 429496729);

$old->start;
$old->safe_psql('postgres',
qq(
	CREATE TABLE test_table (id integer NOT NULL PRIMARY KEY, val text);
	INSERT INTO test_table VALUES (1, 'a');
));

my $conn1 = $old->background_psql('postgres');
my $conn2 = $old->background_psql('postgres');

$conn1->query_safe(qq(
	BEGIN;
	SELECT * FROM test_table WHERE id = 1 FOR SHARE;
));
$conn2->query_safe(qq(
	BEGIN;
	SELECT * FROM test_table WHERE id = 1 FOR SHARE;
));

$conn1->query_safe(qq(COMMIT;));
$conn2->query_safe(qq(COMMIT;));

$conn1->quit;
$conn2->quit;

$old->stop;

# Create new node
my $new = PostgreSQL::Test::Cluster->new("new");
$new->init;

# Run pg_upgrade
command_ok(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $old->data_dir,
		'--new-datadir' => $new->data_dir,
		'--old-bindir' => $old->config_data('--bindir'),
		'--new-bindir' => $new->config_data('--bindir'),
		'--socketdir' => $new->host,
		'--old-port' => $old->port,
		'--new-port' => $new->port,
		$mode,
	],
	'run of pg_upgrade for new instance');
ok( !-d $new->data_dir . "/pg_upgrade_output.d",
	"pg_upgrade_output.d/ removed after pg_upgrade success");

$old->start;
my $src_dump =
	get_dump_for_comparison($old, 'postgres',
							"oldnode_1_dump", 0);
$old->stop;

$new->start;
my $dst_dump =
	get_dump_for_comparison($new, 'postgres',
							"newnode_1_dump", 0);
$new->stop;

compare_files($src_dump, $dst_dump,
	'dump outputs from original and restored regression databases match');

done_testing();
