/*-------------------------------------------------------------------------
 *
 * pgl_sequence_lease.c
 *	  Host-controlled sequence allocation leases for PGlite
 *	  (optimistic-physical-replication design doc §5.3).
 *
 * The JS host grants each cell incarnation a bounded range of sequence
 * values (a lease, coordinated through the stream as setval-shaped G
 * frames).  Allocation authority is the lease, never the sequence page:
 * this module records the per-sequence lease end, and a tiny
 * __PGLITE__-guarded hunk in nextval_internal() clamps the effective
 * MAXVALUE — and therefore the SEQ_LOG_VALS pre-log target — to it for
 * ascending sequences (§5.3 rule 1).  When the lease is exhausted the
 * standard "reached maximum value" error path fires, decorated with the
 * detail "sequence lease exhausted" so the host can distinguish it from a
 * genuine catalog MAXVALUE and renew the grant.
 *
 * Everything here is default-off: with no lease registered, nextval is
 * byte-for-byte vanilla.  Policy (grant sizing, renewal, burn-on-reset)
 * lives in the JS packages; only the clamp mechanism lives here.
 *
 * The build is single-backend (emscripten), so a plain dynahash in
 * TopMemoryContext is sufficient; there is no shared state.
 *
 * Copyright (c) 2026, ElectricSQL
 *
 * IDENTIFICATION
 *	  src/backend/pglite/pgl_sequence_lease.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/sequence.h"
#include "pglite.h"
#include "utils/hsearch.h"

typedef struct PglSequenceLeaseEntry
{
	Oid			seqrelid;		/* hash key: sequence relation OID */
	int64		lease_end;		/* last value this cell may allocate */
	bool		clamped;		/* did the lease reduce maxv on the most
								 * recent nextval_internal() fetch? */
} PglSequenceLeaseEntry;

static HTAB *pgl_sequence_leases = NULL;

static HTAB *
pgl_sequence_lease_hash(void)
{
	if (pgl_sequence_leases == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(PglSequenceLeaseEntry);
		pgl_sequence_leases = hash_create("PGlite sequence leases",
										  16, &ctl,
										  HASH_ELEM | HASH_BLOBS);
	}
	return pgl_sequence_leases;
}

/*
 * pgl_set_sequence_lease
 *		WASM export: register (or update) the lease end for a sequence.
 *		lease_end == 0 clears the lease for that sequence.
 */
void
pgl_set_sequence_lease(Oid seqrelid, int64 lease_end)
{
	if (lease_end == 0)
	{
		if (pgl_sequence_leases != NULL)
			(void) hash_search(pgl_sequence_leases, &seqrelid,
							   HASH_REMOVE, NULL);
	}
	else
	{
		PglSequenceLeaseEntry *entry;
		bool		found;

		entry = (PglSequenceLeaseEntry *)
			hash_search(pgl_sequence_lease_hash(), &seqrelid,
						HASH_ENTER, &found);
		entry->lease_end = lease_end;
		entry->clamped = false;
	}
}

/*
 * pgl_clear_sequence_leases
 *		WASM export: drop every registered lease (vanilla behavior resumes).
 */
void
pgl_clear_sequence_leases(void)
{
	if (pgl_sequence_leases != NULL)
	{
		hash_destroy(pgl_sequence_leases);
		pgl_sequence_leases = NULL;
	}
}

/*
 * pgl_reset_sequence_caches
 *		WASM export: flush the backend-local SeqTable cache (§5.3 rule 3).
 *		Called by the host after every reset-to-head / cell open, since
 *		relfilenumber-keyed invalidation cannot catch replayed foreign
 *		sequence records.
 */
void
pgl_reset_sequence_caches(void)
{
	ResetSequenceCaches();
}

/*
 * PgliteSequenceLeaseClamp
 *		Hook called from nextval_internal() after the catalog bounds are
 *		read.  For an ascending sequence with a registered lease that is
 *		tighter than the catalog MAXVALUE, clamp maxv to the lease end and
 *		disable cycling (wrapping would re-enter ranges leased to other
 *		cells).  Descending sequences and unleased sequences are untouched.
 */
void
PgliteSequenceLeaseClamp(Oid seqrelid, int64 incby, int64 *maxv, bool *cycle)
{
	PglSequenceLeaseEntry *entry;

	if (pgl_sequence_leases == NULL)
		return;

	entry = (PglSequenceLeaseEntry *)
		hash_search(pgl_sequence_leases, &seqrelid, HASH_FIND, NULL);
	if (entry == NULL)
		return;

	if (incby > 0 && entry->lease_end < *maxv)
	{
		*maxv = entry->lease_end;
		*cycle = false;
		entry->clamped = true;
	}
	else
		entry->clamped = false;
}

/*
 * PgliteSequenceLeaseWasClamped
 *		True if the most recent PgliteSequenceLeaseClamp() for this
 *		sequence actually reduced maxv — i.e. a subsequent "reached
 *		maximum value" error is a lease exhaustion, not a genuine catalog
 *		MAXVALUE.  Used to attach the renew-signal errdetail.
 */
bool
PgliteSequenceLeaseWasClamped(Oid seqrelid)
{
	PglSequenceLeaseEntry *entry;

	if (pgl_sequence_leases == NULL)
		return false;

	entry = (PglSequenceLeaseEntry *)
		hash_search(pgl_sequence_leases, &seqrelid, HASH_FIND, NULL);
	return entry != NULL && entry->clamped;
}
