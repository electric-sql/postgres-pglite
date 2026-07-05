/*-------------------------------------------------------------------------
 *
 * pgl_read_wal.c
 *	  Read-cell WAL suppression for PGlite (hardening H2, design doc §14.8).
 *
 * A read-attached (or lazily-attached read) cell only ever seqscans /
 * index-scans replicated relations to serve queries; it must never emit
 * WAL of its own, because its WAL insert position is host-controlled and
 * any stray record would either be captured as spurious slice bytes or
 * corrupt the identity advancement machinery.  The one everyday path that
 * makes an otherwise read-only backend write WAL is opportunistic HOT
 * pruning: heap_page_prune_opt() acquires a cleanup lock and emits an
 * XLOG_HEAP2_PRUNE record when a hot page looks prunable.  Hint bits are
 * already WAL-free here (data checksums are off in the WASM build), so
 * pruning is the only remaining source.
 *
 * This module records a single host-set flag; a tiny __PGLITE__-guarded
 * hunk at the very top of heap_page_prune_opt() returns early when it is
 * set, so the read cell never prunes and therefore never writes WAL.
 *
 * Default off: with the flag unset the guard is a no-op and heap pruning
 * is byte-for-byte vanilla.  The host sets it (via pgl_set_suppress_read_wal)
 * when it opens a cell in a read role, and clears it on write-upgrade.
 *
 * Copyright (c) 2026, ElectricSQL
 *
 * IDENTIFICATION
 *	  src/backend/pglite/pgl_read_wal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pglite.h"

/*
 * Backend-local flag.  Single-backend WASM build, so a plain global is
 * sufficient; there is no shared state and no cross-process concern.
 */
bool		pgl_suppress_read_wal = false;

/*
 * pgl_set_suppress_read_wal
 *		WASM export: enable (nonzero) or disable (0) read-cell WAL
 *		suppression.  While enabled, heap_page_prune_opt() falls out
 *		before it can emit a prune record.
 */
void
pgl_set_suppress_read_wal(int on)
{
	pgl_suppress_read_wal = (on != 0);
}
