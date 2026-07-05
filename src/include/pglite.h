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
extern void pgl_drop_relation_buffers_range(Oid spc_oid, Oid db_oid,
											RelFileNumber rel_number,
											int32 fork_num,
											BlockNumber first_block,
											BlockNumber block_count);
extern void pgl_smgr_release(Oid spc_oid, Oid db_oid,
							 RelFileNumber rel_number);
extern void pgl_smgr_destroy_all(void);

#endif							/* PGLITE_H */
