/*-------------------------------------------------------------------------
 *
 * pgl_apply.c
 *	  Live tail-apply primitives for PGlite followers
 *	  (optimistic-physical-replication design doc §6.2–§6.3, §5.1).
 *
 * A READ-attached cell that must advance past foreign commits can, when
 * the tail is live-appliable, skip the recycle+re-materialize path: the JS
 * host classifies the new WAL slices with pgl_walscan and applies the
 * EAGER special-record set (§6.3) to the LIVE cell through the primitives
 * in this file:
 *
 *   - clog bits (pgl_clog_set / pgl_clog_zero_page) — commit/abort status
 *     is a side effect with no dedicated page record;
 *   - identity advancement (pgl_advance_identity / pgl_advance_xid_past)
 *     replicating the recovery loop's per-record advance semantics (§5.1);
 *   - multixact SLRU records (pgl_multixact_record + zero-page exports) —
 *     SLRU materialization is a semantic pipeline, not page versioning;
 *   - invalidation processing (pgl_process_invals) applied exactly as hot
 *     standby does via ProcessCommittedInvalidationMessages();
 *   - shared/local buffer eviction for blocks the slices rewrote
 *     (pgl_drop_relation_buffers_range over the re-landed
 *     PgliteDropRelationBuffersRange machinery);
 *   - WAL insert-position getters for slice capture.
 *
 * Everything here is host-driven: nothing runs unless the JS host calls
 * it, so an unconfigured build is byte-for-byte vanilla.
 *
 * Provenance: PgliteDropRelationBuffersRange /
 * PgliteFindAndDropRelationBuffersRange / PgliteDropRelationLocalBuffersRange
 * and the WAL-LSN getters re-land the durable-VFS salvage work from the
 * fork branch codex/durable-vfs-postgres (commits 9b2914017d and
 * 5b02971e0b), adapted to the §14.1 mechanism-only surface.
 *
 * Copyright (c) 2026, ElectricSQL
 *
 * IDENTIFICATION
 *	  src/backend/pglite/pgl_apply.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/multixact.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "pglite.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "storage/lwlock.h"
#include "storage/procnumber.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/resowner.h"

/*
 * WAL insert position (salvage: 5b02971e0b).  Split into two uint32s so
 * callers that predate -sWASM_BIGINT keep working; pgl_current_insert_lsn
 * returns the full 64-bit value for BigInt-aware callers.
 */
uint32
pgl_current_wal_insert_lsn_low(void)
{
	return (uint32) GetXLogInsertRecPtr();
}

uint32
pgl_current_wal_insert_lsn_high(void)
{
	return (uint32) (GetXLogInsertRecPtr() >> 32);
}

uint64
pgl_current_insert_lsn(void)
{
	return (uint64) GetXLogInsertRecPtr();
}

/*
 * pgl_process_invals
 *		Apply an array of SharedInvalidationMessages to the live caches
 *		exactly as hot standby applies a committed transaction's
 *		invalidations (§4.3 / §6.3).
 *
 * msgs_ptr points at nmsgs packed SharedInvalidationMessage structs
 * (16 bytes each) in WASM memory — typically the decoded inval payload of
 * a commit / XLOG_INVALIDATIONS / HEAP_INPLACE record as surfaced by
 * pgl_walscan.  dbId/tsId/relcache_init_file_inval come from the same
 * record.
 */
void
pgl_process_invals(uintptr_t msgs_ptr, int nmsgs,
				   bool relcache_init_file_inval,
				   Oid dbId, Oid tsId)
{
	if (nmsgs <= 0 || msgs_ptr == 0)
		return;

	ProcessCommittedInvalidationMessages((SharedInvalidationMessage *) msgs_ptr,
										 nmsgs,
										 relcache_init_file_inval,
										 dbId, tsId);
}

/*
 * Advance latestCompletedXid so that snapshots taken after a foreign
 * commit is applied treat its xid as completed (mirrors what the
 * recovery-to-normal transition derives from nextXid).
 */
static void
pgl_advance_latest_completed(FullTransactionId nextXid)
{
	FullTransactionId latest = nextXid;

	if (!FullTransactionIdIsValid(latest))
		return;
	FullTransactionIdRetreat(&latest);

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	if (!FullTransactionIdIsValid(TransamVariables->latestCompletedXid) ||
		FullTransactionIdPrecedes(TransamVariables->latestCompletedXid,
								  latest))
	{
		TransamVariables->latestCompletedXid = latest;
		TransamVariables->xactCompletionCount++;
	}
	LWLockRelease(ProcArrayLock);
}

/*
 * pgl_advance_identity
 *		Advance-only setters over TransamVariables / MultiXactState with
 *		the same monotonic guards recovery uses (§5.1).  Zero arguments
 *		mean "leave unchanged" (next_full_xid == 0, next_oid == InvalidOid,
 *		next_multi == InvalidMultiXactId).
 */
void
pgl_advance_identity(uint64 next_full_xid, Oid next_oid,
					 MultiXactId next_multi, MultiXactOffset next_offset)
{
	if (next_full_xid != 0)
	{
		FullTransactionId fxid = FullTransactionIdFromU64(next_full_xid);

		if (FullTransactionIdIsNormal(fxid))
		{
			LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
			if (FullTransactionIdPrecedes(TransamVariables->nextXid, fxid))
				TransamVariables->nextXid = fxid;
			LWLockRelease(XidGenLock);
			pgl_advance_latest_completed(TransamVariables->nextXid);
		}
	}

	if (next_oid != InvalidOid)
	{
		/*
		 * Recovery applies XLOG_NEXTOID by overwriting nextOid and zeroing
		 * the prefetch count (xlog_redo); replicate that.
		 */
		LWLockAcquire(OidGenLock, LW_EXCLUSIVE);
		TransamVariables->nextOid = next_oid;
		TransamVariables->oidCount = 0;
		LWLockRelease(OidGenLock);
	}

	if (next_multi != InvalidMultiXactId)
		MultiXactAdvanceNextMXact(next_multi, next_offset);
}

/*
 * pgl_advance_xid_past
 *		The per-record advance a custom applier must replicate (§5.1):
 *		advance nextXid past a replayed record's xl_xid (or a commit
 *		record's payload xids), exactly as recovery's
 *		AdvanceNextFullTransactionIdPastXid does.
 */
void
pgl_advance_xid_past(TransactionId xid)
{
	if (!TransactionIdIsNormal(xid))
		return;

	AdvanceNextFullTransactionIdPastXid(xid);
	pgl_advance_latest_completed(TransamVariables->nextXid);
}

/*
 * pgl_clog_set
 *		Set the clog status of one xid (no subxid tree — the JS applier
 *		iterates a commit record's subxids itself).  status uses the
 *		TRANSACTION_STATUS_* values (1 = committed, 2 = aborted,
 *		3 = sub-committed).
 */
void
pgl_clog_set(TransactionId xid, int status)
{
	if (!TransactionIdIsNormal(xid))
		return;

	TransactionIdSetTreeStatus(xid, 0, NULL,
							   (XidStatus) status, InvalidXLogRecPtr);
	PgliteInvalidateTransactionLogCache();
}

/*
 * pgl_clog_zero_page / pgl_multixact_zero_*_page
 *		Semantic SLRU pipeline primitives (§6.3): apply
 *		CLOG_ZEROPAGE / XLOG_MULTIXACT_ZERO_*_PAGE records to the live
 *		SLRUs the same way their rmgr redo routines do.  The heavy lifting
 *		lives behind tiny wrappers inside clog.c / multixact.c because the
 *		zero-page routines are static there.
 */
void
pgl_clog_zero_page(int64 pageno)
{
	PgliteZeroCLOGPage(pageno);
}

void
pgl_multixact_zero_off_page(int64 pageno)
{
	PgliteZeroMultiXactOffsetsPage(pageno);
}

void
pgl_multixact_zero_mem_page(int64 pageno)
{
	PgliteZeroMultiXactMembersPage(pageno);
}

/*
 * pgl_multixact_record
 *		Apply an XLOG_MULTIXACT_CREATE_ID payload: members_ptr points at
 *		nmembers packed MultiXactMember structs ({uint32 xid, uint32
 *		status}), exactly the layout pgl_walscan surfaces.  The caller is
 *		responsible for the surrounding identity advance
 *		(pgl_advance_identity with mid+1 / moff+nmembers), mirroring
 *		multixact_redo.
 */
void
pgl_multixact_record(MultiXactId mid, MultiXactOffset moff,
					 int nmembers, uintptr_t members_ptr)
{
	if (nmembers < 0 || (nmembers > 0 && members_ptr == 0))
		return;

	PgliteRecordNewMultiXact(mid, moff, nmembers,
							 (MultiXactMember *) members_ptr);
}

/*
 * pgl_invalidate_xact_caches
 *		Drop the backend's one-entry transaction status caches after a
 *		batch of foreign clog/multixact applies.  The SLRU buffers are
 *		deliberately NOT dropped here: semantic applies write THROUGH the
 *		live SLRUs, so their pages are already coherent — and
 *		SimpleLruInvalidateAll discards dirty pages, which would throw
 *		away the very bits just applied.  Wholesale SLRU drops
 *		(pgl_invalidate_slru_caches) exist for the durable-VFS case where
 *		the files were replaced underneath the running instance.
 */
void
pgl_invalidate_xact_caches(void)
{
	PgliteInvalidateTransactionLogCache();
}

/*
 * pgl_invalidate_slru_caches
 *		Wholesale SLRU buffer drop (salvage: 9b2914017d) — ONLY valid when
 *		pg_xact/pg_multixact/pg_subtrans files were replaced externally
 *		and no unwritten local SLRU state must survive: dirty pages are
 *		discarded, not written back.
 */
void
pgl_invalidate_slru_caches(void)
{
	PgliteInvalidateTransactionLogCache();
	PgliteInvalidateCLOGCache();
	PgliteInvalidateSUBTRANSCache();
	PgliteInvalidateMultiXactCache();
}

/*
 * pgl_drop_relation_buffers_range
 *		Evict [first_block, first_block + block_count) of one relation
 *		fork from shared buffers (block_count == 0 means "through end of
 *		fork", which also handles truncation).  Driven by the block refs
 *		of applied records (§6.3 last bullet).
 */
void
pgl_drop_relation_buffers_range(Oid spc_oid, Oid db_oid, RelFileNumber rel_number,
								int32 fork_num, BlockNumber first_block,
								BlockNumber block_count)
{
	RelFileLocator locator;
	SMgrRelation smgr;

	if (fork_num < 0 || fork_num > MAX_FORKNUM)
		return;

	locator.spcOid = spc_oid;
	locator.dbOid = db_oid;
	locator.relNumber = rel_number;

	smgr = smgropen(locator, INVALID_PROC_NUMBER);
	PgliteDropRelationBuffersRange(smgr, (ForkNumber) fork_num,
								   first_block, block_count);
}

/*
 * pgl_smgr_release
 *		Forget cached smgr state (notably the cached relation size) for
 *		one relation, so the next nblocks probe re-lseeks the (possibly
 *		externally grown or truncated) file.  Salvage: 5b02971e0b's
 *		pgl_release_remote_relation.
 */
void
pgl_smgr_release(Oid spc_oid, Oid db_oid, RelFileNumber rel_number)
{
	RelFileLocatorBackend locator_backend;

	locator_backend.locator.spcOid = spc_oid;
	locator_backend.locator.dbOid = db_oid;
	locator_backend.locator.relNumber = rel_number;
	locator_backend.backend = INVALID_PROC_NUMBER;
	smgrreleaserellocator(locator_backend);
}

/*
 * pgl_smgr_destroy_all
 *		Wholesale smgr cache drop (conservative fallback when a slice
 *		touches relations we cannot enumerate cheaply).
 */
void
pgl_smgr_destroy_all(void)
{
	smgrdestroyall();
}

/*
 * ---- M5d rebase validation reads (design doc §4.2) ----
 *
 * Validation must not perturb the evidence: page LSNs are read via
 * BufferGetLSNAtomic on a pinned buffer (never through executor access
 * paths, which would opportunistically prune / set hint bits and
 * interleave stray WAL into the fresh slice), and nblocks comes from the
 * same smgr lseek the capture-side hook records
 * (RelationGetNumberOfBlocksInFork -> smgrnblocks).
 */

/*
 * pgl_page_lsn
 *		Return the page LSN of one block at the CURRENT local state (K
 *		during rebase validation), or UINT64_MAX when the fork/block no
 *		longer exists (§4.2 "missing / truncated page" — the caller
 *		40001s; 0 is a LEGITIMATE LSN on never-WAL-logged initdb pages).
 *		Pin + BufferGetLSNAtomic + unpin under a private resource owner;
 *		runs between transactions.
 */
uint64
pgl_page_lsn(Oid spc_oid, Oid db_oid, RelFileNumber rel_number,
			 int32 fork_num, BlockNumber block_num)
{
	RelFileLocator locator;
	SMgrRelation smgr;
	ResourceOwner owner;
	ResourceOwner old_owner;
	uint64		lsn = PG_UINT64_MAX;

	if (fork_num < 0 || fork_num > MAX_FORKNUM)
		return PG_UINT64_MAX;

	locator.spcOid = spc_oid;
	locator.dbOid = db_oid;
	locator.relNumber = rel_number;

	smgr = smgropen(locator, INVALID_PROC_NUMBER);
	/* Fresh lseek below (cached sizes may predate a live advance). */
	smgrrelease(smgr);
	if (!smgrexists(smgr, (ForkNumber) fork_num))
		return PG_UINT64_MAX;
	if (block_num >= smgrnblocks(smgr, (ForkNumber) fork_num))
		return PG_UINT64_MAX;

	owner = ResourceOwnerCreate(NULL, "pgl_validate");
	old_owner = CurrentResourceOwner;
	CurrentResourceOwner = owner;

	PG_TRY();
	{
		Buffer		buf;

		buf = ReadBufferWithoutRelcache(locator, (ForkNumber) fork_num,
										block_num, RBM_NORMAL, NULL, true);
		lsn = (uint64) BufferGetLSNAtomic(buf);
		ReleaseBuffer(buf);
	}
	PG_CATCH();
	{
		FlushErrorState();
		LWLockReleaseAll();
		UnlockBuffers();
		lsn = PG_UINT64_MAX;
	}
	PG_END_TRY();

	CurrentResourceOwner = owner;
	ResourceOwnerRelease(owner, RESOURCE_RELEASE_BEFORE_LOCKS, false, false);
	ResourceOwnerRelease(owner, RESOURCE_RELEASE_LOCKS, false, false);
	ResourceOwnerRelease(owner, RESOURCE_RELEASE_AFTER_LOCKS, false, false);
	CurrentResourceOwner = old_owner;
	ResourceOwnerDelete(owner);

	return lsn;
}

/*
 * pgl_relation_nblocks
 *		Current nblocks of one relation fork straight from smgr (the same
 *		source the §4.1 capture hook records), with cached sizes dropped
 *		first so the lseek is fresh.  Returns UINT32_MAX when the fork
 *		does not exist (caller treats any mismatch as 40001).
 */
uint32
pgl_relation_nblocks(Oid spc_oid, Oid db_oid, RelFileNumber rel_number,
					 int32 fork_num)
{
	RelFileLocator locator;
	SMgrRelation smgr;
	uint32		result = PG_UINT32_MAX;

	if (fork_num < 0 || fork_num > MAX_FORKNUM)
		return PG_UINT32_MAX;

	locator.spcOid = spc_oid;
	locator.dbOid = db_oid;
	locator.relNumber = rel_number;

	PG_TRY();
	{
		smgr = smgropen(locator, INVALID_PROC_NUMBER);
		smgrrelease(smgr);
		if (smgrexists(smgr, (ForkNumber) fork_num))
			result = (uint32) smgrnblocks(smgr, (ForkNumber) fork_num);
	}
	PG_CATCH();
	{
		FlushErrorState();
		LWLockReleaseAll();
		UnlockBuffers();
		result = PG_UINT32_MAX;
	}
	PG_END_TRY();

	return result;
}
