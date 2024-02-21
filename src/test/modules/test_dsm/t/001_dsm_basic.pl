
# Copyright (c) 2024, PostgreSQL Global Development Group

# Run the standard regression tests with streaming replication
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Basename;

# Initialize primary node
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init();

# Run a SQL command and return psql's stderr (including debug messages)
sub run_dsm_basic_test
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $dynamic_shared_memory_type = shift;
	my $min_dynamic_shared_memory = shift;

	$node->adjust_conf('postgresql.conf', 'dynamic_shared_memory_type', $dynamic_shared_memory_type);
	$node->adjust_conf('postgresql.conf', 'min_dynamic_shared_memory', $min_dynamic_shared_memory);

	$node->start;

	$node->safe_psql('postgres', 'CREATE EXTENSION IF NOT EXISTS test_dsm');

	my $stderr;
	my $cmdret = $node->psql('postgres', 'SELECT test_dsm_basic()', stderr => \$stderr);
	ok($cmdret == 0, "$dynamic_shared_memory_type with minsize $min_dynamic_shared_memory");
	is($stderr, '', "$dynamic_shared_memory_type with minsize $min_dynamic_shared_memory");

	$node->stop('fast');
}

# Test all the DSM implementations

SKIP:
{
	skip "Skipping posix, sysv, mmap test on Windows", 2 if ($windows_os);

	run_dsm_basic_test('posix', 0);
	run_dsm_basic_test('posix', 1000000);
	run_dsm_basic_test('sysv', 0);
	run_dsm_basic_test('sysv', 1000000);
	run_dsm_basic_test('mmap', 0);
	run_dsm_basic_test('mmap', 1000000);
}

SKIP:
{
	skip "Windows dsm support", 2 if (!$windows_os);

	run_dsm_basic_test('windows', 0);
	run_dsm_basic_test('windows', 1000000);
}

done_testing();
