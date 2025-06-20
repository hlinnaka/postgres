#
# Copyright (c) 2025, PostgreSQL Global Development Group
#
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub print_controldata_info
{
	my $node = shift;
	my ($stdout, $stderr) = run_command([ 'pg_controldata', $node->data_dir ]);

	foreach (split("\n", $stdout))
	{
		if ($_ =~ /^Latest checkpoint's Next\s*(.*)$/mg or
			$_ =~ /^Latest checkpoint's oldest\s*(.*)$/mg)
		{
			print $_."\n";
		}
	}
}

# 1) Create the new cluster - source_cluster
my $oldnode = PostgreSQL::Test::Cluster->new('main');
$oldnode->init;

# 2) Reset mxid to the limit
command_ok(
	[ 'pg_resetwal', '-m', '4294967295,4294967295', $oldnode->data_dir ],
	'approaching the mxid limit');
command_ok(
	[ 'pg_resetwal', '-O', '4294967293', $oldnode->data_dir ],
	'approaching the mxoff limit');

# 3) Create files in pg_multixact offsets and members
my $offsets_seg = $oldnode->data_dir . '/pg_multixact/offsets/FFFF';
open my $fh1, '>', $offsets_seg or BAIL_OUT($!);
binmode $fh1;
print $fh1 pack("x[262144]");
close $fh1;

my $members_seg = $oldnode->data_dir . '/pg_multixact/members/14078';
open my $fh2, '>', $members_seg or BAIL_OUT($!);
binmode $fh2;
print $fh2 pack("x[262144]");
close $fh2;

# 4) Create the test table
$oldnode->start;
$oldnode->safe_psql('postgres',
qq(
	CREATE TABLE test_table (id integer NOT NULL PRIMARY KEY, val text);
	INSERT INTO test_table VALUES (1, 'a');
));

# 5-9) Create a multitransaction
my $conn1 = $oldnode->background_psql('postgres');
my $conn2 = $oldnode->background_psql('postgres');

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

$oldnode->stop;

# 10) Run pg_upgrade
my $newnode = PostgreSQL::Test::Cluster->new('new_node');
$newnode->init;

command_ok(
	[
		'pg_upgrade', '--no-sync', '-d', $oldnode->data_dir,
		'-D', $newnode->data_dir, '-b', $oldnode->config_data('--bindir'),
		'-B', $newnode->config_data('--bindir'), '-s', $newnode->host,
		'-p', $oldnode->port, '-P', $newnode->port,
		'--copy',
	],
	'run of pg_upgrade --check for new instance');
ok(!-d $newnode->data_dir . "/pg_upgrade_output.d",
	"pg_upgrade_output.d/ removed after pg_upgrade --check success");

print ">>> OLD NODE: \n";
print_controldata_info($oldnode);

print "\n>>> NEW NODE: \n";
print_controldata_info($newnode);

# just in case...
$newnode->start;
$oldnode->start;
is($newnode->safe_psql('postgres', qq(TABLE test_table;)),
	$oldnode->safe_psql('postgres', qq(TABLE test_table;)),
	'check invariant');
$newnode->stop;
$oldnode->stop;

done_testing();
