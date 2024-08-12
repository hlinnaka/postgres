
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# XXX
# XXX

use IO::Socket::INET;
use IO::Socket::UNIX;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);

# Initialize the server with low connection limits, to test dead-end backends
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "max_connections = 5");
$node->append_conf('postgresql.conf', "log_connections = on");
$node->append_conf('postgresql.conf', "log_min_messages = debug2");

# XX
$node->append_conf('postgresql.conf', "authentication_timeout = '120 s'");

$node->start;

my @sessions = ();
my @raw_connections = ();

#for (my $i=0; $i <= 5; $i++) {
#	push(@sessions, $node->background_psql('postgres', on_error_die => 1));
#}
#$node->connect_fails("dbname=postgres", "max_connections reached",
#					 expected_stderr => qr/FATAL:  sorry, too many clients already/);

# We can still open TCP (or Unix domain socket) connections, but beyond a
# certain number (roughly 2x max_connections), they will be "dead-end backends"
for (my $i = 0; $i <= 20; $i++)
{
	push(@raw_connections, $node->raw_connect());
}

# Test that the dead-end backends don't prevent the server from stopping.
my $before = time();
$node->stop();
my $elapsed = time() - $before;
ok($elapsed < 60);

$node->start();

$node->connect_ok("dbname=postgres", "works after restart");

# Clean up
foreach my $session (@sessions)
{
	$session->quit;
}
foreach my $socket (@raw_connections)
{
	$socket->close();
}

done_testing();
