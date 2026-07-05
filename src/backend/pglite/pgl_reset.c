/*-------------------------------------------------------------------------
 *
 * pgl_reset.c
 *	  In-place reset to base for PGlite cells (M5c, design doc §3.4/§5.1)
 *	  plus the WAL insert-position control and base-identity snapshot
 *	  exports shared with live tail apply.
 *
 * A write cell that loses the CAS race must discard its speculative
 * state at crash-recovery-grade scope (§5.1): shared-memory counters
 * (TransamVariables, MultiXactState), SLRU state, sequence caches,
 * relcache/syscache/plancache, the losing attempt's temp storage, and
 * the WAL insert position — without recycling the instance.
 *
 * Scope decisions (adversarially audited, documented for the record):
 *
 * - Discard alone is NOT crash-recovery-equivalent: crash recovery
 *   re-derives state by REPLAYING WAL from the checkpoint, while an
 *   in-place reset has no replay pass.  A dirty page (or SLRU page) can
 *   mix landed-but-unflushed content with speculative content — its disk
 *   copy predates base, so discarding it would lose landed state
 *   (verified: a never-checkpointed btree page discarded this way reads
 *   back as a zero page).  The design therefore makes base a LOCAL
 *   DURABILITY POINT: pgl_flush_base() (called by the host at every base
 *   snapshot, i.e. after every landed commit) writes out all dirty
 *   shared buffers and SLRU pages.  From then on, disk == base for every
 *   page a speculative attempt can touch, and reset's discard+reread
 *   semantics are exact.
 *
 * - Shared buffers are discarded selectively by page LSN (> base).
 *   Unlogged contamination of <= base pages cannot exist: any
 *   speculative data change advances the page LSN, and speculative
 *   hint-bit / LP_DEAD contamination of <= base pages is impossible
 *   because the host resolves the CAS before any post-commit statement
 *   runs.  SLRUs are dirty-discarded wholesale (zero-and-reread) — sound
 *   because of the flush-at-base invariant — after surgically clearing
 *   the speculative identity ranges [base, current) as belt and braces.
 *   The §5.1 clog-page-boundary hazard is covered: a speculatively
 *   created clog page vanishes from the buffers (its file may remain,
 *   zeroed), and the rewound nextXid re-crosses the boundary later,
 *   re-logging the ZEROPAGE record deterministically in the new slice.
 *
 * - The whole path is gated (JS side) on pgl_storage_writes being
 *   unchanged since the cell's base snapshot (taken AFTER the base
 *   flush): if any page/SLRU/smgr write escaped shared memory since
 *   base, the on-disk state may hold speculative bytes an in-place
 *   reset cannot undo, and the host falls back to recycle.
 *
 * - Temp storage: local buffers are discarded wholesale.  The reset path
 *   only runs for untainted sessions today (temp-tabled sessions keep
 *   the fatal-reset contract on loss), so this is forward mechanism for
 *   the M5d rebase, not a behavior change.
 *
 * Copyright (c) 2026, ElectricSQL
 *
 * IDENTIFICATION
 *	  src/backend/pglite/pgl_reset.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/multixact.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "commands/sequence.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "pglite.h"
#include "storage/bufmgr.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relcache.h"

/* Bumped by smgr.c / slru.c write paths (see pglite.h). */
uint64		pgl_storage_writes = 0;

uint64
pgl_storage_write_count(void)
{
	return pgl_storage_writes;
}

uint64
pgl_get_prev_record_lsn(void)
{
	return (uint64) PgliteGetPrevRecPtr();
}

/*
 * pgl_get_identity
 *		One-call base snapshot for the host: everything a later
 *		pgl_reset_to_base (or live-advance bookkeeping) needs.  Returns a
 *		NUL-terminated JSON string valid until the next call.
 */
uintptr_t
pgl_get_identity(void)
{
	static StringInfoData buf;
	static bool init = false;
	FullTransactionId nextXid;
	Oid			nextOid;
	MultiXactId nextMulti;
	MultiXactOffset nextOffset;
	MemoryContext oldcxt;

	if (!init)
	{
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		initStringInfo(&buf);
		MemoryContextSwitchTo(oldcxt);
		init = true;
	}
	resetStringInfo(&buf);

	LWLockAcquire(XidGenLock, LW_SHARED);
	nextXid = TransamVariables->nextXid;
	LWLockRelease(XidGenLock);
	LWLockAcquire(OidGenLock, LW_SHARED);
	nextOid = TransamVariables->nextOid;
	LWLockRelease(OidGenLock);
	PgliteGetMultiXactIdentity(&nextMulti, &nextOffset);

	appendStringInfo(&buf,
					 "{\"nextXid\":\"" UINT64_FORMAT "\",\"nextOid\":%u,"
					 "\"nextMulti\":%u,\"nextOffset\":%u,"
					 "\"prevRecLsn\":\"" UINT64_FORMAT "\","
					 "\"insertLsn\":\"" UINT64_FORMAT "\","
					 "\"writes\":\"" UINT64_FORMAT "\"}",
					 U64FromFullTransactionId(nextXid),
					 nextOid,
					 nextMulti,
					 nextOffset,
					 (uint64) PgliteGetPrevRecPtr(),
					 (uint64) GetXLogInsertRecPtr(),
					 pgl_storage_writes);

	return (uintptr_t) buf.data;
}

/*
 * pgl_flush_base
 *		Make the current position a local durability point: write out
 *		every dirty shared buffer and all SLRU pages (no checkpoint
 *		record, no control-file update — pure page flush).  Called by
 *		the host right before it snapshots the base identity, so a later
 *		pgl_reset_to_base can rely on disk == base.  Returns 0 on
 *		success, -1 if called inside a transaction, -2 on error.
 */
int
pgl_flush_base(void)
{
	volatile int rc = 0;

	if (IsTransactionState())
		return -1;

	PG_TRY();
	{
		CheckPointBuffers(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE);
		CheckPointCLOG();
		CheckPointSUBTRANS();
		CheckPointMultiXact();
	}
	PG_CATCH();
	{
		FlushErrorState();
		LWLockReleaseAll();
		rc = -2;
	}
	PG_END_TRY();

	return rc;
}

/*
 * pgl_flush_wal
 *		XLogFlush up to the current insert position.  Slice capture must
 *		never read a torn tail from pg_wal: commit records of temp-only
 *		transactions ride the ASYNC commit path (XLogSetAsyncXactLSN — no
 *		flush, and single-backend PGlite has no walwriter to catch up),
 *		so the host flushes explicitly before reading WAL bytes.
 */
int
pgl_flush_wal(void)
{
	volatile int rc = 0;

	PG_TRY();
	{
		XLogFlush(GetXLogInsertRecPtr());
	}
	PG_CATCH();
	{
		FlushErrorState();
		LWLockReleaseAll();
		rc = -1;
	}
	PG_END_TRY();

	return rc;
}

/*
 * pgl_set_wal_position
 *		Host-callable wrapper over PgliteSetWalPosition (xlog.c): point
 *		the insert machinery at end_of_log with xl_prev = last_rec.
 *		Returns 1 on success, 0 on failure.
 */
int
pgl_set_wal_position(uint64 end_of_log, uint64 last_rec)
{
	if (IsTransactionState())
		return 0;
	return PgliteSetWalPosition((XLogRecPtr) end_of_log,
								(XLogRecPtr) last_rec) ? 1 : 0;
}

/*
 * pgl_reset_to_base
 *		Discard the cell's speculative state and rewind to base.
 *
 * Returns 0 on success; on any nonzero return the caller MUST recycle
 * the cell (state may be part-mutated):
 *	 -1 called inside a transaction,
 *	 -2 a pinned speculative buffer exists,
 *	 -3 an internal error was caught,
 *	 -4 the WAL position rewind failed.
 */
int
pgl_reset_to_base(uint64 base_lsn, uint64 prev_rec_lsn,
				  uint64 base_next_full_xid, Oid base_next_oid,
				  MultiXactId base_next_multi,
				  MultiXactOffset base_next_offset)
{
	FullTransactionId baseXid = FullTransactionIdFromU64(base_next_full_xid);
	FullTransactionId curXid;
	FullTransactionId latest;
	volatile int rc = 0;

	if (IsTransactionState())
		return -1;
	if (base_lsn == 0 || prev_rec_lsn == 0 ||
		!FullTransactionIdIsNormal(baseXid) ||
		base_next_oid == InvalidOid)
		return -1;

	PG_TRY();
	{
		/* 1. Speculative shared buffers (LSN > base), discard-no-write. */
		if (PgliteDiscardBuffersAboveLsn((XLogRecPtr) base_lsn) < 0)
			rc = -2;

		if (rc == 0)
		{
			/* 2. Temp storage of the losing attempt (all local buffers). */
			PgliteDiscardAllLocalBuffers();

			/* 3. SLRU speculative ranges + identity rewind (§5.1 scope). */
			LWLockAcquire(XidGenLock, LW_SHARED);
			curXid = TransamVariables->nextXid;
			LWLockRelease(XidGenLock);

			if (FullTransactionIdPrecedes(baseXid, curXid))
			{
				PgliteClogClearRange(XidFromFullTransactionId(baseXid),
									 XidFromFullTransactionId(curXid));
				PgliteSubTransClearRange(XidFromFullTransactionId(baseXid),
										 XidFromFullTransactionId(curXid));
			}
			PgliteResetNextMultiXact(base_next_multi, base_next_offset);

			/*
			 * Wholesale SLRU dirty-discard (zero-and-reread): sound
			 * because disk == base (pgl_flush_base ran at snapshot and
			 * the write counter is unchanged since — JS gate).
			 */
			PgliteInvalidateCLOGCache();
			PgliteInvalidateSUBTRANSCache();
			PgliteInvalidateMultiXactCache();

			LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
			TransamVariables->nextXid = baseXid;
			LWLockRelease(XidGenLock);

			latest = baseXid;
			FullTransactionIdRetreat(&latest);
			LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
			TransamVariables->latestCompletedXid = latest;
			TransamVariables->xactCompletionCount++;
			LWLockRelease(ProcArrayLock);

			LWLockAcquire(OidGenLock, LW_EXCLUSIVE);
			TransamVariables->nextOid = base_next_oid;
			TransamVariables->oidCount = 0;
			LWLockRelease(OidGenLock);

			PgliteInvalidateTransactionLogCache();

			/* 4. Sequence caches (§5.3 rule 3, mandatory). */
			ResetSequenceCaches();

			/*
			 * 5. Full relcache/syscache/plancache flush — the simple safe
			 * default (§3.4) — honoring relcacheInitFileInval by unlinking
			 * the init files around it.
			 */
			RelationCacheInitFilePreInvalidate();
			InvalidateSystemCaches();
			RelationCacheInitFilePostInvalidate();

			/* 6. Cached relation sizes / fds. */
			smgrdestroyall();
		}
	}
	PG_CATCH();
	{
		FlushErrorState();
		LWLockReleaseAll();
		rc = -3;
	}
	PG_END_TRY();

	if (rc != 0)
		return rc;

	/* 7. WAL insert-position rewind: next record lands at exactly base. */
	if (!PgliteSetWalPosition((XLogRecPtr) base_lsn,
							  (XLogRecPtr) prev_rec_lsn))
		return -4;

	return 0;
}
