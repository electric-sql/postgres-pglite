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

#endif							/* PGLITE_H */
