
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that a shared invalidation catchup request is not lost, if it arrives
# while the backend is still processing a previous catchup request.
#
# When a backend falls far behind in the shared invalidation queue,
# SICleanupQueue() sends it a catchup signal and sets its "signaled" flag.
# The backend clears the flag when it has caught up, and as long as it is
# set, no further catchup signals are sent to it.  If a new signal arrives
# after the backend has caught up and cleared the flag, but before it has
# finished processing the previous request, the new request must not be
# lost.  Otherwise the backend is left with the flag set but no pending
# request, and won't be sent any more catchup signals while it stays idle.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init();

# The idle session below should be the backend furthest behind in the
# invalidation queue, so that it's the one that gets sent catchup requests.
# Keep autovacuum out of the way.
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

# Start the session that we'll observe.  It logs at DEBUG4 so that we can see
# when it processes catchup requests.  Attach the injection point locally,
# so that only this session waits on it.  After the setup, the session sits
# idle, waiting for a command from the client, which is when it processes
# catchup requests.
my $idle = $node->background_psql('postgres');
my $idle_pid = $idle->query_safe('SELECT pg_backend_pid();');
$idle->query_safe(
	q{
	SET log_min_messages = debug4;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('sinval-caught-up', 'wait');
});

my $sent_re = qr/sending sinval catchup signal to PID $idle_pid\b/;
my $processed_re = qr/\[$idle_pid\] .*DEBUG:  ProcessCatchupEvent/;

# Count the catchup requests sent to the idle session so far.
sub requests_sent
{
	my $log = slurp_file($node->logfile);
	my $count = () = $log =~ /$sent_re/g;
	return $count;
}

# Generate invalidation messages, in batches, until the idle session has been
# sent at least $target catchup requests.  The sending backend logs at DEBUG4
# too, so that we can see the requests in the log.
sub generate_invals_until_sent
{
	my ($target) = @_;

	for (my $i = 0; $i < 100; $i++)
	{
		return 1 if requests_sent() >= $target;
		$node->safe_psql(
			'postgres', q{
			SET log_min_messages = debug4;
			DO $$
			BEGIN
				FOR i IN 1..100 LOOP
					EXECUTE 'CREATE TABLE sinval_t (a int)';
					EXECUTE 'DROP TABLE sinval_t';
				END LOOP;
			END $$;
		});
	}
	return requests_sent() >= $target;
}

# Make the idle session fall behind, so that it gets a catchup request.  It
# catches up, and then stops at the injection point, before it has finished
# processing the request.
ok(generate_invals_until_sent(1),
	'idle session was sent a catchup request');
$node->wait_for_event('client backend', 'sinval-caught-up');

# While it's stopped there, make it fall behind again, so that it gets a
# second catchup request.  It has already caught up, so it's eligible for a
# new request.
ok(generate_invals_until_sent(2),
	'idle session was sent a second catchup request while processing the first'
);

# Let it continue.  Detach first, so that it doesn't stop again.  It should
# now process the second request too.
my $log_offset = -s $node->logfile;
$node->safe_psql(
	'postgres', q{
	SELECT injection_points_detach('sinval-caught-up');
	SELECT injection_points_wakeup('sinval-caught-up');
});
$node->wait_for_log($processed_re, $log_offset);
pass('idle session processed the second catchup request');

# Once it has caught up again, it should get further catchup requests as
# usual.  If the second request had been lost, its "signaled" flag would
# still be set, and it would never be sent another one.
ok(generate_invals_until_sent(3),
	'idle session was sent a catchup request again');

# The session is still usable.
is($idle->query_safe('SELECT 1;'), '1', 'idle session still works');
$idle->quit;

done_testing();
