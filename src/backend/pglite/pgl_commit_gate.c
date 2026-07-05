/*-------------------------------------------------------------------------
 *
 * pgl_commit_gate.c
 *	  The M5e commit gate (design doc §3.6/§14.2): defer the one
 *	  irreversible pre-commit step — the ON COMMIT DELETE ROWS temp-table
 *	  truncate — past the host's CAS verdict.
 *
 * MECHANISM DECISION (the §14.2 "one deep hunk", as built).  The design
 * doc sketched a C hook blocking inside COMMIT on a SAB/Atomics bridge
 * awaiting the CAS.  That shape is unsound in the shipped topology: the
 * cell's WASM, the committer, the tailer, and (in tests and the
 * single-process tier) the stream gateway itself share ONE Node event
 * loop, so a synchronous JS import parked on Atomics.wait freezes the
 * very loop that must execute the CAS — deadlock by construction.  A
 * literal mid-COMMIT park (early-return split of CommitTransaction /
 * RecordTransactionCommit) was also rejected: it would split two
 * functions across a WAL critical section AND break the M5d rebase
 * ladder, which is architecturally premised on local-commit-before-
 * capture (harvest = post-commit same-session reads).
 *
 * The shipped gate instead observes that under M5c's in-place reset the
 * local commit is ALREADY provisional: every §3.6 pre-commit step except
 * one is reversible when the CAS verdict arrives after the local commit
 * (deferred triggers / clog / ProcArray — rewound by the reset; NOTIFY —
 * harvested in JS, dies with the attempt; LO close — harmless; holdable
 * portals — closed from JS on loss).  The ONLY irreversible step is the
 * physical heap_truncate of ON COMMIT DELETE ROWS temp tables.  So the
 * gate defers exactly that:
 *
 *   - armed (per cell, default off = vanilla): PreCommit_on_commit_actions
 *     hands its oids_to_truncate list to PgliteDeferOnCommitTruncates
 *     instead of truncating (one tiny hunk in tablecmds.c);
 *   - CAS lands  -> the host calls pgl_commit_gate_run(): the truncates
 *     execute in their own transaction — strictly AFTER the verdict;
 *   - CAS lost   -> the host calls pgl_commit_gate_discard() (and
 *     pgl_reset_to_base discards as belt and braces): the truncate never
 *     happened, so the client's temp staging data survives the loss —
 *     the §3.6 reorder, achieved.
 *
 * Copyright (c) 2026, ElectricSQL
 *
 * IDENTIFICATION
 *	  src/backend/pglite/pgl_commit_gate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/heap.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pglite.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* Gate armed for this cell (pgl_commit_gate_set; default off = vanilla). */
static bool gate_enabled = false;

/*
 * True while pgl_commit_gate_run drives the deferred truncates: its own
 * little transaction's commit re-enters PreCommit_on_commit_actions,
 * which must then truncate inline (vanilla) rather than re-defer.
 */
static bool gate_running = false;

/* Deferred ON COMMIT DELETE ROWS relids (TopMemoryContext). */
static List *pending_truncates = NIL;

/*
 * PgliteDeferOnCommitTruncates
 *		Called from PreCommit_on_commit_actions (the tablecmds.c hunk)
 *		with the oids_to_truncate list.  Returns true iff the truncate
 *		was captured for post-CAS execution (caller skips heap_truncate).
 */
bool
PgliteDeferOnCommitTruncates(List *relids)
{
	MemoryContext oldcxt;
	ListCell   *lc;

	if (!gate_enabled || gate_running)
		return false;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	foreach(lc, relids)
		pending_truncates = list_append_unique_oid(pending_truncates,
												   lfirst_oid(lc));
	MemoryContextSwitchTo(oldcxt);
	return true;
}

/* Arm / disarm the gate for this cell.  Disarming keeps any pendings. */
void
pgl_commit_gate_set(int on)
{
	gate_enabled = (on != 0);
}

/* Number of deferred truncates awaiting a verdict. */
int
pgl_commit_gate_pending(void)
{
	return list_length(pending_truncates);
}

/*
 * pgl_commit_gate_discard
 *		The loss verdict: the local commit is being reversed (in-place
 *		reset or recycle), so the deferred truncates must never run.
 *		Also called from pgl_reset_to_base as belt and braces.
 */
void
pgl_commit_gate_discard(void)
{
	list_free(pending_truncates);
	pending_truncates = NIL;
}

/*
 * pgl_commit_gate_run
 *		The landed verdict: execute the deferred truncates in their own
 *		transaction (heap_truncate needs one).  Relations dropped since
 *		the deferring commit are skipped.  Returns 0 on success (or
 *		nothing pending), -1 if called inside a transaction, -2 on error
 *		(pendings retained; the caller must treat the cell as poisoned
 *		and recycle it).
 */
int
pgl_commit_gate_run(void)
{
	volatile int rc = 0;

	if (pending_truncates == NIL)
		return 0;
	if (IsTransactionState())
		return -1;

	gate_running = true;
	PG_TRY();
	{
		List	   *live = NIL;
		ListCell   *lc;

		StartTransactionCommand();
		foreach(lc, pending_truncates)
		{
			Oid			relid = lfirst_oid(lc);

			if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
				live = lappend_oid(live, relid);
		}
		if (live != NIL)
		{
			/* index rebuilds may evaluate expressions (cf. the
			 * oids_to_drop branch in PreCommit_on_commit_actions) */
			PushActiveSnapshot(GetTransactionSnapshot());
			heap_truncate(live);
			PopActiveSnapshot();
		}
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		FlushErrorState();
		AbortCurrentTransaction();
		rc = -2;
	}
	PG_END_TRY();
	gate_running = false;

	if (rc == 0)
		pgl_commit_gate_discard();
	return rc;
}
