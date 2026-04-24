/*-------------------------------------------------------------------------
 *
 * prockill_race.c
 *		SQL helpers for TAP test t/001_prockill_lockgroup_injection.pl
 *
 * Exposes lock-group formation without parallel query so ProcKill lock-group
 * teardown can be stress-tested with injection points.
 *
 * Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/prockill_race/prockill_race.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/wait_event.h"

#include "../injection_points/injection_point_condition.h"

PG_MODULE_MAGIC;

/*
 * Read a uint32 field exactly once.  Matches the idiom used elsewhere (see
 * src/backend/utils/adt/waitfuncs.c) for sampling PGPROC->wait_event_info
 * without locking.
 */
#define UINT32_ACCESS_ONCE(var)  ((uint32) (*((volatile uint32 *)&(var))))

PG_FUNCTION_INFO_V1(prockill_become_lock_group_leader);

Datum
prockill_become_lock_group_leader(PG_FUNCTION_ARGS)
{
	BecomeLockGroupLeader();
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(prockill_become_lock_group_member);

Datum
prockill_become_lock_group_member(PG_FUNCTION_ARGS)
{
	int			leader_pid = PG_GETARG_INT32(0);
	PGPROC	   *leader;

	leader = BackendPidGetProc(leader_pid);
	if (leader == NULL)
		elog(ERROR, "backend with PID %d not found", leader_pid);

	if (!BecomeLockGroupMember(leader, leader_pid))
		elog(ERROR, "could not join lock group of backend %d", leader_pid);

	PG_RETURN_VOID();
}

/**
 * @brief Attach injection_wait() to a named INJECTION_POINT, scoped to a
 *        single victim PID, from a controller backend.
 *
 * @details
 * Registers the injection_wait callback from the sibling injection_points
 * module against point_name, carrying an InjectionPointCondition of type
 * INJ_CONDITION_PID and pid = target_pid as private data.  Every backend
 * that later reaches INJECTION_POINT(point_name, NULL) runs the callback,
 * but only the one whose MyProcPid equals target_pid actually sleeps on
 * the point's condition variable — until injection_points_wakeup(point_name)
 * is called from some other session.
 *
 * Why this helper exists (the important part):
 *
 * The TAP scenario needs to pause the victim backends (leader and follower)
 * inside ProcKill() at the prockill-after-lockgroup-* sites in proc.c.  The
 * naive approach would be for the victim itself to run
 * injection_points_set_local() + injection_points_attach(name, 'wait')
 * before it is terminated.  That does not work — and the failure mode is
 * not obvious at first:
 *
 *   - injection_points_set_local() flips injection_point_local = true and
 *     registers before_shmem_exit(injection_points_cleanup, 0).
 *   - injection_points_attach() in local mode appends the attached point
 *     name to inj_list_local so that injection_points_cleanup() knows to
 *     detach it on process exit.
 *   - On pg_terminate_backend() the victim goes into proc_exit().
 *     before_shmem_exit callbacks run before shmem_exit, and shmem_exit is
 *     what eventually calls ProcKill (registered via on_shmem_exit).  So
 *     injection_points_cleanup() fires first and calls
 *     InjectionPointDetach() for every locally-tracked point.
 *   - By the time ProcKill() actually runs in the victim, the injection
 *     point is gone; INJECTION_POINT(prockill-after-lockgroup-*) resolves
 *     to a no-op, the victim never waits, and the TAP test has no way to
 *     control the order in which the two victims traverse ProcKill —
 *     defeating the whole point of the reproducer.
 *
 * Attaching via this function from a controller backend (a third session
 * that is not being terminated) avoids the trap: the controller never
 * enters ProcKill during the scenario, its own eventual shutdown is
 * unrelated to the victims', and — crucially — we call
 * InjectionPointAttach() directly, without going through
 * injection_points_set_local().  No before_shmem_exit cleanup is registered
 * on the victims, so the injection point remains live in shared memory
 * throughout each victim's exit, including while they are executing
 * ProcKill().  The PID condition keeps the wait surgical: only the intended
 * victim blocks; every other backend that happens to traverse the same code
 * path runs the callback but returns immediately.
 *
 * Additional properties worth noting:
 *
 *   - The injection_points extension must be preloaded
 *     (shared_preload_libraries = 'injection_points') so that
 *     injection_wait and its condition handling are resolvable from every
 *     backend, including the victim at the moment it runs ProcKill().
 *   - The callback metadata lives in shared memory via
 *     InjectionPointAttach(), so it is visible to all backends regardless
 *     of which session performed the attach.
 *   - The action is hard-coded to "injection_wait".  This module has no
 *     use for the "error" / "notice" actions, and the narrower API keeps
 *     the TAP call sites short and hard to misuse.
 *   - Detaching is the caller's responsibility (the TAP test calls
 *     injection_points_detach(name) during cleanup).  If the maximum
 *     number of attached points is reached, InjectionPointAttach() raises
 *     ERROR; this function does not try to paper over that.
 *
 * @param[in] point_name  Name of the injection point as it appears in the
 *                        backend source (the first argument of
 *                        INJECTION_POINT()).  SQL text.  Must be non-NULL;
 *                        the function is declared STRICT.
 * @param[in] target_pid  OS PID of the victim backend that should block
 *                        when it executes point_name.  SQL integer.  Other
 *                        backends that reach the same point run the
 *                        callback but return immediately, without sleeping.
 *
 * @return SQL void — the function is called for its side effect (registering
 *         the callback in shared memory).  On any failure inside
 *         InjectionPointAttach() control does not return normally; an
 *         ERROR is raised instead.
 *
 * @note Must be called from a backend that is not one of the victims.
 *       Intended for use only from TAP tests of this module.
 */
PG_FUNCTION_INFO_V1(prockill_attach_injection_wait);

Datum
prockill_attach_injection_wait(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			target_pid = PG_GETARG_INT32(1);
	InjectionPointCondition cond;

	cond.type = INJ_CONDITION_PID;
	cond.pid = target_pid;

	InjectionPointAttach(name, "injection_points", "injection_wait",
						 &cond, sizeof(cond));
	PG_RETURN_VOID();
}

/**
 * @brief Test-only probe: is the backend with the given PID currently
 *        waiting on the named injection point, read directly from its
 *        PGPROC slot?
 *
 * @details
 * Scans ProcGlobal->allProcs for the slot whose proc->pid equals target_pid,
 * atomically reads proc->wait_event_info (see UINT32_ACCESS_ONCE), resolves
 * it to a wait-event name via pgstat_get_wait_event(), and returns whether
 * that name equals point_name.
 *
 * Why this helper exists (the important part):
 *
 * The TAP scenario that drives this module needs to observe a leader /
 * follower backend exactly while it is suspended at an INJECTION_POINT
 * placed inside ProcKill() during lock-group teardown.  None of the standard
 * observability surfaces work at that point in the exit sequence:
 *
 *   - pg_stat_activity returns nothing for the target because the pgstat
 *     BackendStatusArray slot has already been cleared by
 *     pgstat_beshutdown_hook (it sets st_procpid = 0, and
 *     pgstat_read_current_status filters on st_procpid > 0).
 *   - BackendPidGetProc() returns NULL because the target has already been
 *     removed from the ProcArray by RemoveProcFromArray().
 *
 * Both of those cleanups are registered as on_shmem_exit callbacks and
 * therefore run, in LIFO order, before ProcKill()'s own body — that is,
 * before our injection point fires.  Attempts to use pg_stat_activity or
 * BackendPidGetProc() to detect the wait therefore spin until they time
 * out.
 *
 * What is still valid at the injection point is the PGPROC slot itself,
 * because ProcKill() only zeroes proc->pid and proc->wait_event_info near
 * the end of its body — after all INJECTION_POINT macros inside it.
 * Scanning ProcGlobal->allProcs directly therefore yields a truthful,
 * non-racy answer at exactly the moment we care about.
 *
 * The read of wait_event_info is deliberately lock-free and single-word
 * atomic (mirroring pg_stat_get_backend_wait_event_type /
 * pg_stat_get_backend_wait_event in waitfuncs.c).  A torn read is not
 * possible because the field is a 4-byte aligned uint32.
 *
 * Limitations:
 *
 *   - If two PGPROC slots claim the same PID (e.g. briefly, while a slot is
 *     being recycled) the first match wins.  This is not a concern for the
 *     test scenario because the target PIDs are known-live and unique.
 *   - point_name must be the name as returned by pgstat_get_wait_event()
 *     for the PG_WAIT_INJECTIONPOINT class, which is exactly the name
 *     passed to INJECTION_POINT() in the backend code under test.
 *
 * @param[in] target_pid  OS PID of the backend to inspect (SQL integer).
 * @param[in] point_name  Injection-point / wait-event name to match
 *                        (SQL text).  Must be non-NULL; the function is
 *                        declared STRICT.
 *
 * @retval true   A PGPROC with proc->pid equal to target_pid exists and
 *                its current wait-event name equals point_name.
 * @retval false  Either no PGPROC has proc->pid == target_pid, or it does
 *                exist but is not currently reporting any wait, or it is
 *                waiting on a different event.
 *
 * @note Intended for use only from TAP tests of this module.  The lookup is
 *       O(MaxBackends + NUM_AUXILIARY_PROCS) per call.
 */
PG_FUNCTION_INFO_V1(prockill_backend_in_injection);

Datum
prockill_backend_in_injection(PG_FUNCTION_ARGS)
{
	int			target_pid = PG_GETARG_INT32(0);
	char	   *want_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	uint32		n = ProcGlobal->allProcCount;

	for (uint32 i = 0; i < n; i++)
	{
		PGPROC	   *proc = &ProcGlobal->allProcs[i];
		uint32		wei;
		const char *ev;

		if (proc->pid != target_pid)
			continue;

		wei = UINT32_ACCESS_ONCE(proc->wait_event_info);
		ev = pgstat_get_wait_event(wei);

		PG_RETURN_BOOL(ev != NULL && strcmp(ev, want_name) == 0);
	}

	PG_RETURN_BOOL(false);
}
