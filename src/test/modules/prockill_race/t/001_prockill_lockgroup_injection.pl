# Copyright (c) 2025-2026, PostgreSQL Global Development Group
#
# Deterministic regression for ProcKill lock-group teardown vs. shared
# procLatch ownership (bug: PGPROC recycled while procLatch still marked owned).
# Requires -Dinjection_points=true, TAP tests, and temp-install (setup suite).
#
# Injection point names: prockill-after-lockgroup-* in proc.c (INJECTION_POINT).
#
# The test expects a fixed server: no "latch already owned" PANIC, both
# victims reach their respective ProcKill injection points, and the
# controller's final SELECT 1 succeeds.  Any of those failing indicates a
# regression of the lock-group teardown fix.
#
# The wait/wakeup primitives are NOT injection_points's injection_wait: in the
# fixed ProcKill, SwitchBackToLocalLatch / pgstat_reset_wait_event_storage /
# DisownLatch all run above the INJECTION_POINT, which breaks every
# procLatch-based wait.  Instead the prockill_race module provides a
# shared-memory poll loop via prockill_injection_present() /
# prockill_injection_wakeup().
#
# Why eval around some calls: safe_psql dies on connection errors.  If the
# lock-group fix regresses, the postmaster may PANIC mid-scenario and later
# psql invocations will fail to connect; we catch those failures so we can
# still classify the outcome via the server log and shut the node down
# cleanly.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils qw(slurp_file);
use Test::More;
use PostgreSQL::Test::Cluster;

use constant PANIC_RE => qr/PANIC:.*latch already owned by PID/s;

##################
# Initialization #
##################
if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('prockill_race');
$node->init;
$node->append_conf('postgresql.conf',
	q{shared_preload_libraries = 'injection_points,prockill_race'});
$node->start;

plan skip_all => 'Extension injection_points not installed'
  unless $node->check_extension('injection_points');
plan skip_all => 'Extension prockill_race not installed'
  unless $node->check_extension('prockill_race');

$node->safe_psql('postgres',
	q{CREATE EXTENSION IF NOT EXISTS injection_points; CREATE EXTENSION IF NOT EXISTS prockill_race;});

my $leader = $node->background_psql('postgres');
my $follower = $node->background_psql('postgres');

#################################################
# Part 1. Create lock group leader and a member #
#################################################
$leader->query_safe('SELECT prockill_become_lock_group_leader();');
my $leader_pid = $leader->query_safe('SELECT pg_backend_pid()');
$leader_pid =~ s/\s+//g;

my $follower_pid = $follower->query_safe('SELECT pg_backend_pid()');
$follower_pid =~ s/\s+//g;

$follower->query_safe(
	"SELECT prockill_become_lock_group_member($leader_pid);");

# Attach the injection-point waits from a *separate*, one-shot controller
# session -- never from $leader or $follower, and never from a session that
# has run injection_points_set_local().
#
# Why a controller session:
#
#   * The ProcKill injection points must still be attached when the victim
#     backends (leader/follower) tear themselves down.  If the attach were
#     done via injection_points_set_local()+injection_points_attach() from
#     the victim itself, the before_shmem_exit(injection_points_cleanup)
#     callback registered by set_local() would fire ahead of shmem_exit
#     (and therefore ahead of ProcKill, which is on_shmem_exit), detach
#     the point mid-exit, and turn INJECTION_POINT(prockill-after-
#     lockgroup-*) into a no-op.  The victim would never wait, and the
#     race-ordering handshake this test relies on would collapse.
#
#   * prockill_attach_injection_wait() calls InjectionPointAttach() directly,
#     without touching injection_points_set_local().  Running it from a
#     non-victim backend therefore leaves no before_shmem_exit cleanup hook
#     on the leader or follower: the injection points stay live in shared
#     memory across the victims' entire exit path, including across
#     ProcKill().
#
#   * safe_psql() spawns a fresh psql (a "throwaway controller") for this
#     one call.  That backend exits cleanly right after attaching; its own
#     eventual proc_exit is unrelated to the leader/follower teardown we
#     are about to drive.  Using $leader or $follower here instead would
#     both (a) reintroduce the set_local()/before_shmem_exit trap above
#     and (b) tie the attach's lifetime to the session we are about to
#     terminate.
#
# The PID arguments pick which victim each injection point matches:
# INJ_CONDITION_PID in the callback's private data keys on MyProcPid, so
# only the named leader_pid / follower_pid actually sleep; any other
# backend that happens to hit the same INJECTION_POINT runs the callback
# and returns immediately.
$node->safe_psql(
	'postgres', qq(
	SELECT prockill_attach_injection_wait('prockill-after-lockgroup-leader', $leader_pid);
	SELECT prockill_attach_injection_wait('prockill-after-lockgroup-follower', $follower_pid);
));

#######################################################################
# Part 2. Terminate the locks group leader and the member concurently #
#######################################################################
$node->safe_psql('postgres',
	"SELECT pg_terminate_backend($leader_pid)");

my $leader_wait_ok =
  wait_for_injection_present($node, 'prockill-after-lockgroup-leader');

eval {
	$node->safe_psql('postgres',
		"SELECT pg_terminate_backend($follower_pid)");
};

my $follower_wait_ok =
  wait_for_injection_present($node, 'prockill-after-lockgroup-follower');

# Release the follower first.  Order matters: waking the leader first would
# risk it completing ProcKill's freelist push before the follower reaches the
# second INJECTION_POINT, hiding the bug under the fix.
eval {
	$node->safe_psql('postgres',
		q{SELECT prockill_injection_wakeup('prockill-after-lockgroup-follower');});
};

eval {
	$node->safe_psql('postgres',
		q{SELECT prockill_injection_wakeup('prockill-after-lockgroup-leader');});
};

##########################################################
# Part 3. Looking for "latch already owned by PID" panic #
##########################################################
my $survived = eval {
	$node->safe_psql('postgres', 'SELECT 1');
	1;
};
my $select_err = $@;

# Was the latch-recycle PANIC reached?  Either the postmaster log already
# recorded it, or our last SELECT died with that specific error text.
my $log = eval { slurp_file($node->logfile) } // '';
my $panic = ($log =~ PANIC_RE)
  || (defined $select_err
	&& $select_err ne ''
	&& $select_err =~ /latch already owned by PID/);

my ($outcome_ok, $outcome_desc);
if ($panic)
{
	$outcome_ok   = 0;
	$outcome_desc = 'latch recycle PANIC (regression of ProcKill lock-group fix)';
}
elsif ($survived)
{
	$outcome_ok   = 1;
	$outcome_desc =
		'ProcKill lock-group: no latch recycle PANIC and session survived';
}
else
{
	$outcome_ok   = 0;
	$outcome_desc =
		'neither PANIC nor successful SELECT 1 (harness or environment)';
}

ok($outcome_ok, $outcome_desc);

ok(
	!$panic && ($leader_wait_ok && $follower_wait_ok),
	'leader and follower reached ProcKill injection waits without latch PANIC'
);

eval {
	$node->safe_psql('postgres',
		q{SELECT injection_points_detach('prockill-after-lockgroup-leader');});
	$node->safe_psql('postgres',
		q{SELECT injection_points_detach('prockill-after-lockgroup-follower');});
};

############
# Clean up #
############
eval { $leader->quit; };
eval { $follower->quit; };

my $stop_fail_ok =
	 $panic
	|| !-e ($node->data_dir . '/postmaster.pid');
if ($stop_fail_ok)
{
	$node->stop('fast', fail_ok => 1);
}
else
{
	$node->stop('fast');
}

done_testing();


# Wait until some backend has reported presence at the named injection point,
# i.e. until prockill_injection_present() starts returning true.  The
# attachment is PID-scoped by prockill_attach_injection_wait(), so in practice
# the reporter is always the specific victim backend that entered ProcKill.
#
# Bounded loop.  We stop polling early if safe_psql dies with a signature of
# "server is gone" (connection refused, closed mid-query, or the latch-recycle
# PANIC itself): if the lock-group fix regresses the postmaster may crash and
# shut down before we get here, in which case every subsequent poll would
# just be a wasted psql fork/exec.  Classification of that outcome is left to
# the caller (via the postmaster log / $@ on later statements).
sub wait_for_injection_present
{
	my ($node, $injection_name) = @_;
	my $n = $injection_name;
	$n =~ s/'/''/g;
	# 200 * 50ms ~= 10s per phase.
	for my $i (1 .. 200)
	{
		my $saw = eval {
			$node->safe_psql('postgres',
				"SELECT prockill_injection_present('$n')")
		};
		my $err = $@;
		return 1 if defined $saw && $saw eq 't';
		last
		  if $err
		  && $err =~
		  /could not connect|server closed the connection|latch already owned/;
		select undef, undef, undef, 0.05;
	}
	return 0;
}
