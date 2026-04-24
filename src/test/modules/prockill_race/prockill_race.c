/*-------------------------------------------------------------------------
 *
 * prockill_race.c
 *		SQL helpers for TAP test t/001_prockill_lockgroup_injection.pl
 *
 * Exposes lock-group formation without parallel query so ProcKill lock-group
 * teardown can be stress-tested with injection points, and provides a small
 * wait primitive that the TAP test uses in place of the injection_points
 * module's injection_wait callback.
 *
 * Why a custom wait primitive: injection_points's injection_wait suspends the
 * victim via ConditionVariableSleep, which relies on MyLatch / procLatch and
 * on MyProc->wait_event_info.  In the fixed ProcKill, SwitchBackToLocalLatch,
 * pgstat_reset_wait_event_storage and DisownLatch all run at the very top of
 * the function -- before the INJECTION_POINT in the lock-group block.  That
 * means at the injection site: MyLatch has been switched to LocalLatchData
 * (broadcasts still SetLatch(&proc->procLatch) and miss the waiter),
 * proc->procLatch.owner_pid is 0 (SetLatch on an unowned latch no-ops), and
 * MyProc->wait_event_info is no longer populated.  A latch-based wait would
 * hang and would be undetectable; a shared-memory flag polled via pg_usleep
 * side-steps all three of those torn-down subsystems.
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
#include "storage/s_lock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"

#include "../injection_points/injection_point_condition.h"

PG_MODULE_MAGIC;

/*
 * One slot per injection-point name.  The controller session calls
 * prockill_attach_injection_wait() to register (name, target_pid) and attach
 * our callback to the injection point.  The victim backend, on reaching the
 * INJECTION_POINT inside ProcKill, runs prockill_test_wait(), which sets
 * `present_pid` to its MyProcPid and then polls `wakeup`.  A second
 * controller session calls prockill_injection_present() to detect presence
 * and prockill_injection_wakeup() to release the waiter.
 */
#define PROCKILL_N_POINTS		2
#define PROCKILL_POINT_NAME_MAX 64

typedef struct ProckillSlot
{
	char		name[PROCKILL_POINT_NAME_MAX];
	int			target_pid;
	int			present_pid;
	bool		wakeup;
} ProckillSlot;

typedef struct ProckillSharedState
{
	slock_t		lock;
	ProckillSlot slots[PROCKILL_N_POINTS];
} ProckillSharedState;

static ProckillSharedState *prockill_state = NULL;

/*
 * Shared memory lifecycle.  We deliberately use the main shared memory
 * segment (ShmemRequestStruct + RegisterShmemCallbacks) rather than a DSM
 * segment, because our wait callback runs from inside on_shmem_exit
 * (ProcKill) -- which fires AFTER shmem_exit has already invoked
 * dsm_backend_shutdown() and unmapped all DSM segments.  Touching a DSM
 * segment from ProcKill would be a use-after-unmap; the main shmem segment
 * is still valid all the way through the on_shmem_exit phase.
 *
 * That in turn requires prockill_race to be listed in
 * shared_preload_libraries so that _PG_init() runs in the postmaster and
 * RegisterShmemCallbacks() takes the preloaded path.  The TAP test does this.
 */
static void
prockill_shmem_request(void *arg)
{
	ShmemRequestStruct(.name = "prockill_race",
					   .size = sizeof(ProckillSharedState),
					   .ptr = (void **) &prockill_state);
}

static void
prockill_shmem_init(void *arg)
{
	ProckillSharedState *s = prockill_state;

	SpinLockInit(&s->lock);
	memset(s->slots, 0, sizeof(s->slots));
}

static const ShmemCallbacks prockill_shmem_callbacks = {
	.request_fn = prockill_shmem_request,
	.init_fn = prockill_shmem_init,
};

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	RegisterShmemCallbacks(&prockill_shmem_callbacks);
}

/*
 * Look up a slot by name.  Returns index in slots[], or -1 on miss.
 * Caller must hold prockill_state->lock.
 */
static int
prockill_find_slot(const char *name)
{
	for (int i = 0; i < PROCKILL_N_POINTS; i++)
	{
		if (prockill_state->slots[i].name[0] != '\0' &&
			strcmp(prockill_state->slots[i].name, name) == 0)
			return i;
	}
	return -1;
}

/*
 * Look up a slot by name, or return the first free slot.  Returns -1 only if
 * neither a match nor a free slot exists.  Caller must hold
 * prockill_state->lock.
 */
static int
prockill_find_or_alloc_slot(const char *name)
{
	int			idx = prockill_find_slot(name);

	if (idx >= 0)
		return idx;
	for (int i = 0; i < PROCKILL_N_POINTS; i++)
	{
		if (prockill_state->slots[i].name[0] == '\0')
			return i;
	}
	return -1;
}

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
 * @brief Injection-point callback that suspends the victim backend on a
 *        shared-memory flag, polling via pg_usleep instead of a latch.
 *
 * @details
 * Attached by prockill_attach_injection_wait() below.  Invoked from the
 * INJECTION_POINT() expansion inside ProcKill.  Only the backend whose
 * MyProcPid matches the InjectionPointCondition's pid actually sleeps; every
 * other backend that reaches the same INJECTION_POINT runs the callback and
 * returns immediately.
 *
 * Why this callback exists instead of reusing injection_points's
 * injection_wait: the fixed ProcKill moves SwitchBackToLocalLatch,
 * pgstat_reset_wait_event_storage and DisownLatch above the INJECTION_POINT.
 * After that, any ConditionVariable-based wait (including injection_wait)
 * cannot be woken: ConditionVariableBroadcast issues SetLatch on
 * proc->procLatch, but that latch has owner_pid == 0 so SetLatch returns
 * without signalling; MyLatch meanwhile points at LocalLatchData whose
 * is_set is never touched.  The victim would block in epoll_wait forever.
 * Polling a slock_t-protected bool via pg_usleep's select(2) does not
 * depend on any of that machinery and works regardless of latch state.
 *
 * The slot for `name` is expected to have been created already by the
 * controller's prockill_attach_injection_wait() call; missing the slot is an
 * ERROR (test harness bug).
 *
 * @param[in] name          Injection-point name.  Used to look up the slot.
 * @param[in] private_data  Pointer to an InjectionPointCondition struct
 *                          (type INJ_CONDITION_PID, pid = target victim).
 * @param[in] arg           Optional INJECTION_POINT argument (unused).
 */
PGDLLEXPORT void prockill_test_wait(const char *name,
									const void *private_data,
									void *arg);

void
prockill_test_wait(const char *name, const void *private_data, void *arg)
{
	const InjectionPointCondition *cond = private_data;
	int			idx;

	/*
	 * The attach registers INJ_CONDITION_PID(target_pid).  Only that one
	 * backend should actually sleep; every other backend that reaches this
	 * INJECTION_POINT returns immediately.
	 */
	if (MyProcPid != cond->pid)
		return;

	SpinLockAcquire(&prockill_state->lock);
	idx = prockill_find_slot(name);
	if (idx < 0)
	{
		SpinLockRelease(&prockill_state->lock);
		elog(ERROR,
			 "prockill_test_wait: no slot for injection point \"%s\"",
			 name);
	}
	prockill_state->slots[idx].present_pid = MyProcPid;
	SpinLockRelease(&prockill_state->lock);

	for (;;)
	{
		bool		wake;

		SpinLockAcquire(&prockill_state->lock);
		wake = prockill_state->slots[idx].wakeup;
		SpinLockRelease(&prockill_state->lock);
		if (wake)
			break;
		pg_usleep(5000L);		/* 5 ms */
	}

	SpinLockAcquire(&prockill_state->lock);
	prockill_state->slots[idx].present_pid = 0;
	SpinLockRelease(&prockill_state->lock);
}

/**
 * @brief Register (name, target_pid) and attach prockill_test_wait to the
 *        matching INJECTION_POINT.
 *
 * @details
 * Allocates or reuses a shared-memory slot for point_name, resets its
 * wakeup / present_pid fields, and calls InjectionPointAttach() so that the
 * next time any backend executes INJECTION_POINT(point_name, ...) the
 * prockill_test_wait callback runs.
 *
 * Why attach from a dedicated controller session (not from the victim):
 * the ordinary injection_points session API (injection_points_set_local() +
 * injection_points_attach()) registers before_shmem_exit(
 * injection_points_cleanup), which would fire during shmem_exit ahead of
 * ProcKill (on_shmem_exit) and detach the point before INJECTION_POINT()
 * reaches it -- turning the callback into a no-op.  We deliberately call
 * InjectionPointAttach() directly, without going through set_local(), so a
 * non-victim backend can install the wait and the point stays live across
 * the victims' entire exit path, including across ProcKill().  The PID
 * argument is required because attachment happens in a backend whose
 * MyProcPid is irrelevant to the condition -- the condition must refer to
 * the victim's PID.
 *
 * @param[in] point_name  Name of the INJECTION_POINT the victim will hit.
 * @param[in] target_pid  OS PID of the victim backend that should block at
 *                        the point.  Other backends that reach the same
 *                        INJECTION_POINT pass through without sleeping.
 * @return SQL void.
 */
PG_FUNCTION_INFO_V1(prockill_attach_injection_wait);

Datum
prockill_attach_injection_wait(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			target_pid = PG_GETARG_INT32(1);
	InjectionPointCondition cond;
	int			idx;

	if (strlen(name) >= PROCKILL_POINT_NAME_MAX)
		ereport(ERROR,
				(errmsg("injection point name too long (max %d bytes)",
						PROCKILL_POINT_NAME_MAX - 1)));

	SpinLockAcquire(&prockill_state->lock);
	idx = prockill_find_or_alloc_slot(name);
	if (idx < 0)
	{
		SpinLockRelease(&prockill_state->lock);
		ereport(ERROR,
				(errmsg("no free prockill_race wait slot (max %d)",
						PROCKILL_N_POINTS)));
	}
	strlcpy(prockill_state->slots[idx].name, name,
			PROCKILL_POINT_NAME_MAX);
	prockill_state->slots[idx].target_pid = target_pid;
	prockill_state->slots[idx].present_pid = 0;
	prockill_state->slots[idx].wakeup = false;
	SpinLockRelease(&prockill_state->lock);

	cond.type = INJ_CONDITION_PID;
	cond.pid = target_pid;

	InjectionPointAttach(name, "prockill_race", "prockill_test_wait",
						 &cond, sizeof(cond));
	PG_RETURN_VOID();
}

/**
 * @brief Has any backend reported presence at the named injection point?
 *
 * @details
 * Returns true iff some backend's prockill_test_wait() has set
 * slots[idx].present_pid != 0 for point_name.  Because attachment is
 * PID-scoped (only the target victim actually sleeps), presence effectively
 * means "the target has reached the INJECTION_POINT and entered the polling
 * loop".
 *
 * Used by the TAP harness instead of PGPROC->wait_event_info inspection,
 * because pgstat_reset_wait_event_storage() has already redirected wait-event
 * storage to a process-local variable by the time our callback runs.
 *
 * @param[in] point_name  Injection-point name.
 * @return true if a backend is currently sleeping inside prockill_test_wait
 *         for that point, false otherwise (including "no such slot").
 */
PG_FUNCTION_INFO_V1(prockill_injection_present);

Datum
prockill_injection_present(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		present = false;
	int			idx;

	SpinLockAcquire(&prockill_state->lock);
	idx = prockill_find_slot(name);
	if (idx >= 0)
		present = (prockill_state->slots[idx].present_pid != 0);
	SpinLockRelease(&prockill_state->lock);

	PG_RETURN_BOOL(present);
}

/**
 * @brief Release the waiter sleeping inside prockill_test_wait for a point.
 *
 * @details
 * Sets slots[idx].wakeup = true for the named slot.  The waiter's poll loop
 * will observe this on its next pg_usleep wake-up (within ~5 ms) and return.
 * Errors if no slot for point_name exists; that would indicate a test
 * ordering bug.
 *
 * @param[in] point_name  Injection-point name whose waiter should be released.
 * @return SQL void.
 */
PG_FUNCTION_INFO_V1(prockill_injection_wakeup);

Datum
prockill_injection_wakeup(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			idx;

	SpinLockAcquire(&prockill_state->lock);
	idx = prockill_find_slot(name);
	if (idx < 0)
	{
		SpinLockRelease(&prockill_state->lock);
		ereport(ERROR,
				(errmsg("prockill_race: no slot for injection point \"%s\"",
						name)));
	}
	prockill_state->slots[idx].wakeup = true;
	SpinLockRelease(&prockill_state->lock);

	PG_RETURN_VOID();
}
