/*-------------------------------------------------------------------------
 *
 * pgl_readset.c
 *	  Read-set capture for transparent rebase validation
 *	  (optimistic-physical-replication design doc §4.1).
 *
 * One tiny hunk at PinBufferForBlock() — the single choke point every
 * physical page read passes through, including PG18 read_stream/AIO —
 * appends (rel, fork, block) to a fixed ring here whenever capture is
 * enabled.  A second tiny hunk at RelationGetNumberOfBlocksInFork()
 * records nblocks probes (seqscan page sets are frozen from an smgr
 * lseek, not a page read — §4.1's verified false negative without this).
 *
 * MECHANISM ONLY: the host enables capture per transaction and harvests
 * at commit; the rebase validator that consumes the snapshot is JS (M5d).
 * Exclusions applied here: local (temp) buffers never reach the shared
 * PinBufferForBlock path with capture semantics we want, and FSM forks
 * are not WAL-logged — both are skipped.  Sequence-page exclusion is a
 * JS-side filter (relkind is not visible at bufmgr level).
 *
 * Capture disabled (default): the hunk is a single predictable branch —
 * vanilla behavior.
 *
 * Copyright (c) 2026, ElectricSQL
 *
 * IDENTIFICATION
 *	  src/backend/pglite/pgl_readset.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/relpath.h"
#include "pglite.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/*
 * Entry layout (packed uint32 x5, mirrored by the JS harvester):
 *   [0] spcOid  [1] dbOid  [2] relNumber
 *   [3] kind << 24 | fork      (kind 0 = page pin, 1 = nblocks probe)
 *   [4] block number (kind 0) or nblocks (kind 1)
 */
#define PGL_READSET_FIELDS		5
#define PGL_READSET_CAPACITY	65536

static uint32 pgl_readset_buf[PGL_READSET_CAPACITY * PGL_READSET_FIELDS];
static uint32 pgl_readset_n = 0;
static bool pgl_readset_on = false;
static bool pgl_readset_overflow = false;

bool		pgl_readset_enabled = false;	/* fast-path flag read by hunks */

static inline void
pgl_readset_push(const RelFileLocator *rlocator, uint32 kind_fork,
				 uint32 value)
{
	uint32	   *e;

	if (pgl_readset_n >= PGL_READSET_CAPACITY)
	{
		pgl_readset_overflow = true;
		return;
	}
	e = pgl_readset_buf + (pgl_readset_n * PGL_READSET_FIELDS);
	e[0] = rlocator->spcOid;
	e[1] = rlocator->dbOid;
	e[2] = rlocator->relNumber;
	e[3] = kind_fork;
	e[4] = value;
	pgl_readset_n++;
}

/* Hunk entry: every shared-buffer page pin (PinBufferForBlock). */
void
PgliteReadSetRecordPin(const RelFileLocator *rlocator, int fork_num,
					   BlockNumber block_num)
{
	if (!pgl_readset_on)
		return;
	if (fork_num == FSM_FORKNUM)	/* not WAL-logged — excluded (§4.1) */
		return;
	pgl_readset_push(rlocator, (uint32) fork_num, block_num);
}

/* Hunk entry: nblocks probes (RelationGetNumberOfBlocksInFork). */
void
PgliteReadSetRecordNblocks(const RelFileLocator *rlocator, int fork_num,
						   BlockNumber nblocks)
{
	if (!pgl_readset_on)
		return;
	pgl_readset_push(rlocator, (1u << 24) | (uint32) fork_num, nblocks);
}

/* ---- WASM exports ---- */

void
pgl_readset_enable(int on)
{
	pgl_readset_on = (on != 0);
	pgl_readset_enabled = pgl_readset_on;
	pgl_readset_n = 0;
	pgl_readset_overflow = false;
}

void
pgl_readset_reset(void)
{
	pgl_readset_n = 0;
	pgl_readset_overflow = false;
}

/* Number of captured entries (each PGL_READSET_FIELDS uint32s). */
uint32
pgl_readset_count(void)
{
	return pgl_readset_n;
}

/* 1 if the ring filled and entries were dropped (validator must 40001). */
int
pgl_readset_overflowed(void)
{
	return pgl_readset_overflow ? 1 : 0;
}

/* Base pointer of the packed entry array in WASM memory. */
uintptr_t
pgl_readset_snapshot(void)
{
	return (uintptr_t) pgl_readset_buf;
}
