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

CREATE FUNCTION prockill_attach_injection_wait(point_name text, target_pid integer)
RETURNS void
AS 'MODULE_PATHNAME', 'prockill_attach_injection_wait'
LANGUAGE C STRICT PARALLEL UNSAFE;

-- Test-only probe: is the backend with the given PID currently waiting on the
-- named injection point?  Looks directly at ProcGlobal->allProcs so it keeps
-- working while the target is blocked inside ProcKill() (after pgstat and
-- ProcArray teardown have already run).
CREATE FUNCTION prockill_backend_in_injection(target_pid integer, point_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'prockill_backend_in_injection'
LANGUAGE C STRICT PARALLEL UNSAFE;
