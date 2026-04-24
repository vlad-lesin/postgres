/* prockill_race--1.0.sql */

\echo Use "CREATE EXTENSION prockill_race" to load this file. \quit

CREATE FUNCTION prockill_become_lock_group_leader()
RETURNS void
AS 'MODULE_PATHNAME', 'prockill_become_lock_group_leader'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION prockill_become_lock_group_member(leader_pid integer)
RETURNS void
AS 'MODULE_PATHNAME', 'prockill_become_lock_group_member'
LANGUAGE C STRICT PARALLEL UNSAFE;

-- Register (point_name, target_pid) in module shared memory and attach the
-- prockill_test_wait callback to the INJECTION_POINT.  The wait is
-- deliberately independent of procLatch / MyLatch / wait_event_info because
-- ProcKill tears those down before the injection point fires.
CREATE FUNCTION prockill_attach_injection_wait(point_name text, target_pid integer)
RETURNS void
AS 'MODULE_PATHNAME', 'prockill_attach_injection_wait'
LANGUAGE C STRICT PARALLEL UNSAFE;

-- Test-only probe: is some backend currently sleeping inside
-- prockill_test_wait for the named injection point?  Attachment is
-- PID-scoped, so in practice this is true iff the target victim reached the
-- point and entered the polling loop.
CREATE FUNCTION prockill_injection_present(point_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'prockill_injection_present'
LANGUAGE C STRICT PARALLEL UNSAFE;

-- Release the waiter (if any) sleeping inside prockill_test_wait for the
-- named injection point.  Flips a shared-memory flag that the waiter polls.
CREATE FUNCTION prockill_injection_wakeup(point_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'prockill_injection_wakeup'
LANGUAGE C STRICT PARALLEL UNSAFE;
