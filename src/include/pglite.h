/*-------------------------------------------------------------------------
 *
 * pglite.h
 *	  PGlite host hook interface (design doc §14.1).
 *
 *	  This header enumerates the pgl_* mechanism surface the PGlite fork
 *	  exposes to the JS host, plus the internal hook entry points called
 *	  from tiny __PGLITE__-guarded hunks in existing Postgres files.
 *	  Every hook is default-off and vanilla-behaving when unconfigured:
 *	  policy lives in the JS packages, only mechanism lives here.
 *
 * Copyright (c) 2026, ElectricSQL
 *
 * src/include/pglite.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGLITE_H
#define PGLITE_H

#include "access/xlogdefs.h"
#include "common/relpath.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/*
 * Sequence leases (design doc §5.3, src/backend/pglite/pgl_sequence_lease.c)
 *
 * The host registers a per-sequence allocation lease end.  While a lease
 * is registered for an ascending sequence, nextval_internal() clamps its
 * effective MAXVALUE (and therefore the SEQ_LOG_VALS pre-log target) to
 * the lease end; exhausting the lease raises the standard
 * ERRCODE_SEQUENCE_GENERATOR_LIMIT_EXCEEDED error carrying the detail
 * "sequence lease exhausted", which the host treats as the renew signal.
 * With no lease registered (the default), behavior is exactly vanilla.
 */

/* WASM exports (host-callable) */
extern void pgl_set_sequence_lease(Oid seqrelid, int64 lease_end);
extern void pgl_clear_sequence_leases(void);
extern void pgl_reset_sequence_caches(void);

/* Internal hook entry points (called from commands/sequence.c) */
extern void PgliteSequenceLeaseClamp(Oid seqrelid, int64 incby,
									 int64 *maxv, bool *cycle);
extern bool PgliteSequenceLeaseWasClamped(Oid seqrelid);

/*
 * WAL-range scanner (design doc §14.2, src/backend/pglite/pgl_walscan.c)
 *
 * pgl_walscan_begin([start, end), tli) then pgl_walscan_next() per
 * record: returns a pointer to a NUL-terminated JSON classification of
 * the record (valid until the next call), 0 when exhausted.  The eager
 * special-record set (§6.3) carries decoded payloads.
 */
extern int	pgl_walscan_begin(uint64 start, uint64 end, uint32 tli);
extern uintptr_t pgl_walscan_next(void);
extern int	pgl_walscan_block_image(int block_id, uintptr_t dst);
extern void pgl_walscan_end_scan(void);

/*
 * Live tail-apply primitives (design doc §6.3/§5.1,
 * src/backend/pglite/pgl_apply.c).  Salvage provenance:
 * codex/durable-vfs-postgres 9b2914017d + 5b02971e0b.
 */
extern uint32 pgl_current_wal_insert_lsn_low(void);
extern uint32 pgl_current_wal_insert_lsn_high(void);
extern uint64 pgl_current_insert_lsn(void);
extern void pgl_process_invals(uintptr_t msgs_ptr, int nmsgs,
							   bool relcache_init_file_inval,
							   Oid dbId, Oid tsId);
extern void pgl_advance_identity(uint64 next_full_xid, Oid next_oid,
								 MultiXactId next_multi,
								 MultiXactOffset next_offset);
extern void pgl_advance_xid_past(TransactionId xid);
extern void pgl_clog_set(TransactionId xid, int status);
extern void pgl_clog_zero_page(int64 pageno);
extern void pgl_multixact_zero_off_page(int64 pageno);
extern void pgl_multixact_zero_mem_page(int64 pageno);
extern void pgl_multixact_record(MultiXactId mid, MultiXactOffset moff,
								 int nmembers, uintptr_t members_ptr);
extern void pgl_invalidate_xact_caches(void);
extern void pgl_invalidate_slru_caches(void);
extern void pgl_drop_relation_buffers_range(Oid spc_oid, Oid db_oid,
											RelFileNumber rel_number,
											int32 fork_num,
											BlockNumber first_block,
											BlockNumber block_count);
extern void pgl_smgr_release(Oid spc_oid, Oid db_oid,
							 RelFileNumber rel_number);
extern void pgl_smgr_destroy_all(void);

/*
 * Single-record redo harness (design doc §14.2 "page materialization",
 * M5c; src/backend/pglite/pgl_walscan.c).  Applies the CURRENT walscan
 * record's block changes to the LIVE cell through the resident rmgr's
 * rm_redo, for a whitelist of buffer-only rmgrs.  Returns 1 = applied,
 * 0 = record not whitelisted, negative = error (caller must fall back
 * to recycle).  Only sound between transactions with the local WAL
 * insert position already advanced to cover the record (page LSNs set
 * by redo must be flushable).
 */
extern int	pgl_walscan_redo_current(void);
extern int	pgl_redo_whitelisted(int rmid, int info);

/* True while pgl_walscan_redo_current drives an rm_redo call. */
extern PGDLLIMPORT bool pgl_redo_active;

/*
 * In-place reset + WAL position control (design doc §3.4/§5.1, M5c;
 * src/backend/pglite/pgl_reset.c + __PGLITE__ hunks in xlog.c etc.)
 */
extern uintptr_t pgl_get_identity(void);
extern uint64 pgl_get_prev_record_lsn(void);
extern uint64 pgl_storage_write_count(void);
extern int	pgl_flush_base(void);
extern int	pgl_flush_wal(void);
extern int	pgl_set_wal_position(uint64 end_of_log, uint64 last_rec);
extern int	pgl_reset_to_base(uint64 base_lsn, uint64 prev_rec_lsn,
							  uint64 base_next_full_xid, Oid base_next_oid,
							  MultiXactId base_next_multi,
							  MultiXactOffset base_next_offset);

/* Internal hook entry points backing the reset (various files). */
extern bool PgliteSetWalPosition(XLogRecPtr endOfLog, XLogRecPtr lastRec);
extern XLogRecPtr PgliteGetPrevRecPtr(void);
extern int	PgliteDiscardBuffersAboveLsn(XLogRecPtr cutoff);
extern void PgliteDiscardAllLocalBuffers(void);
extern void PgliteClogClearRange(TransactionId from, TransactionId to);
extern void PgliteSubTransClearRange(TransactionId from, TransactionId to);
extern void PgliteResetNextMultiXact(MultiXactId multi,
									 MultiXactOffset offset);
extern void PgliteGetMultiXactIdentity(MultiXactId *next_multi,
									   MultiXactOffset *next_offset);

/* Storage write counter (bumped in smgr.c / slru.c write paths). */
extern PGDLLIMPORT uint64 pgl_storage_writes;

/*
 * M5e commit gate (design doc §3.6/§14.2,
 * src/backend/pglite/pgl_commit_gate.c): defers the ON COMMIT DELETE
 * ROWS truncate — the ONE irreversible pre-commit step under reset-based
 * verdict handling — past the host's CAS verdict.  pgl_commit_gate_run
 * executes pendings post-verdict in their own transaction;
 * pgl_commit_gate_discard drops them on loss (also called from
 * pgl_reset_to_base as belt and braces).  Default off = vanilla.
 */
struct List;					/* avoid dragging nodes/pg_list.h here */
extern void pgl_commit_gate_set(int on);
extern int	pgl_commit_gate_pending(void);
extern int	pgl_commit_gate_run(void);
extern void pgl_commit_gate_discard(void);
extern bool PgliteDeferOnCommitTruncates(struct List *relids);

/*
 * Flush every dirty local buffer (localbuf.c): part of pgl_flush_base's
 * local-durability-point contract, so an in-place reset's wholesale
 * local-buffer discard re-reads a tainted session's PRE-ATTEMPT
 * temp-table content exactly (M5e taint lift).
 */
extern void PgliteFlushAllLocalBuffers(void);

/*
 * Read-set capture (design doc §4.1, src/backend/pglite/pgl_readset.c)
 *
 * Entries are packed uint32 x5: spc, db, rel, kind<<24|fork, block —
 * kind 0 = page pin (PinBufferForBlock), kind 1 = nblocks probe.
 */
extern void pgl_readset_enable(int on);
extern void pgl_readset_reset(void);
extern uint32 pgl_readset_count(void);
extern int	pgl_readset_overflowed(void);
extern uintptr_t pgl_readset_snapshot(void);

/*
 * Rebase validation reads (design doc §4.2, M5d;
 * src/backend/pglite/pgl_apply.c): pinned-buffer page-LSN peek (never an
 * executor path) and fresh smgr nblocks.  pgl_page_lsn returns 0 when
 * the page is missing/truncated; pgl_relation_nblocks returns UINT32_MAX
 * for a missing fork.
 */
extern uint64 pgl_page_lsn(Oid spc_oid, Oid db_oid, RelFileNumber rel_number,
						   int32 fork_num, BlockNumber block_num);
extern uint32 pgl_relation_nblocks(Oid spc_oid, Oid db_oid,
								   RelFileNumber rel_number, int32 fork_num);

/* Internal hook entry points (called from storage/buffer/bufmgr.c) */
extern PGDLLIMPORT bool pgl_readset_enabled;
extern void PgliteReadSetRecordPin(const RelFileLocator *rlocator,
								   int fork_num, BlockNumber block_num);
extern void PgliteReadSetRecordNblocks(const RelFileLocator *rlocator,
									   int fork_num, BlockNumber nblocks);

/*
 * Read-cell WAL suppression (hardening H2, design doc §14.8;
 * src/backend/pglite/pgl_read_wal.c).  When enabled, a __PGLITE__ hunk at
 * the top of heap_page_prune_opt() returns early so a read-attached cell
 * never opportunistically prunes and therefore never writes WAL.  Default
 * off = vanilla pruning.
 */
extern PGDLLIMPORT bool pgl_suppress_read_wal;
extern void pgl_set_suppress_read_wal(int on);

#endif							/* PGLITE_H */
