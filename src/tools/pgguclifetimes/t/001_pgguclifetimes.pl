# pgguclifetimes: some simple tests

# Copyright (c) 2023, PostgreSQL Global Development Group

use strict;
use warnings;

use Cwd qw(getcwd);

use PostgreSQL::Test::Utils;
use Test::More;

my $workdir = getcwd;

program_help_ok('pgguclifetimes');
program_options_handling_ok('pgguclifetimes');

command_fails(
	['pgguclifetimes'],
	'pgguclifetimes without a compilation database arg');

command_fails(
	['pgguclifetimes', "$workdir/does-not-exist"],
	'pgguclifetimes with a non-existent compilation database directory');

# command_ok(
# 	['pgguclifetimes', "$workdir/tests/success"],
# 	'pgguclifetimes exits with 0 after analyzing a fully annotated codebase');

command_checks_all(
	['pgguclifetimes', "$workdir/tests/failure"],
	1,
	[qr/^$/],
	[
		qr/src\/tools\/pgguclifetimes\/tests\/failure\/example.c:1:5: needs_all is missing a lifetime annotation/,
		qr/src\/tools\/pgguclifetimes\/tests\/failure\/example.c:2:12: static_needs_all is missing a lifetime annotation/,
		qr/src\/tools\/pgguclifetimes\/tests\/failure\/example.c:3:12: extern_needs_all is missing a lifetime annotation/,
	],
	'pgguclifetimes analyzing a non-annotated codebase'
);

done_testing();
