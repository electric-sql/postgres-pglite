/*-------------------------------------------------------------------------
 *
 * pgl_walscan.c
 *	  Export-driven WAL-range scanner for the PGlite JS host
 *	  (optimistic-physical-replication design doc §14.2, §4.3, §6.3).
 *
 * The JS tailer hands a [start, end) LSN range whose bytes are present in
 * the cell's pg_wal (the VFS serves them — they need not be known to the
 * local XLOG insert state, so we read segment files directly instead of
 * going through read_local_xlog_page).  pgl_walscan_next() returns one
 * classified record per call as a JSON string: the per-record loop is C
 * (xlogreader — battle-tested parsing for free), classification and
 * orchestration are JS (§14.2 layer rule).
 *
 * Every record carries {lsn, end, rmid, info, xid, blocks[]}; the EAGER
 * special-record set (§6.3) additionally carries decoded payloads:
 * commit/abort (subxids + inval messages + relfilelocator drops),
 * XLOG_INVALIDATIONS, HEAP_INPLACE invals, multixact CREATE_ID /
 * zero-page / truncate, XLOG_SEQ_LOG relfilelocator, NEXTOID, SMGR
 * create/truncate, DBASE create/drop, clog zero/truncate, and
 * checkpoints (identity fields).
 *
 * Inval message arrays are surfaced as hex so the host can pass the raw
 * bytes back to pgl_process_invals() unchanged; multixact members are
 * surfaced both decoded and as hex of the packed MultiXactMember array
 * for pgl_multixact_record().
 *
 * Copyright (c) 2026, ElectricSQL
 *
 * IDENTIFICATION
 *	  src/backend/pglite/pgl_walscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/clog.h"
#include "access/heapam_xlog.h"
#include "access/multixact.h"
#include "access/rmgr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "catalog/pg_control.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands_xlog.h"
#include "commands/sequence.h"
#include "lib/stringinfo.h"
#include "pglite.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/resowner.h"
#include "utils/json.h"
#include "utils/memutils.h"

/* Scanner state (single backend — plain statics). */
static XLogReaderState *pglws_reader = NULL;
static XLogRecPtr pglws_end = InvalidXLogRecPtr;
static TimeLineID pglws_tli = 1;
static int	pglws_fd = -1;
static XLogSegNo pglws_segno = 0;
static StringInfoData pglws_json;
static bool pglws_json_init = false;

/*
 * Page-read callback: serve XLOG_BLCKSZ pages straight from the pg_wal
 * segment files (pg_waldump-style), independent of local flush state.
 */
static int
pglws_read_page(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
				XLogRecPtr targetPtr, char *readBuf)
{
	XLogSegNo	segno;
	uint32		offset;
	int			nread;

	XLByteToSeg(targetPagePtr, segno, wal_segment_size);

	if (pglws_fd < 0 || segno != pglws_segno)
	{
		char		path[MAXPGPATH];

		if (pglws_fd >= 0)
		{
			close(pglws_fd);
			pglws_fd = -1;
		}
		XLogFilePath(path, pglws_tli, segno, wal_segment_size);
		pglws_fd = BasicOpenFile(path, O_RDONLY | PG_BINARY);
		if (pglws_fd < 0)
			return -1;
		pglws_segno = segno;
	}

	offset = XLogSegmentOffset(targetPagePtr, wal_segment_size);
	nread = (int) pread(pglws_fd, readBuf, XLOG_BLCKSZ, (off_t) offset);
	if (nread < reqLen)
		return -1;
	return nread;
}

/*
 * pgl_walscan_begin
 *		Start a scan of [start, end).  start must be a record boundary
 *		(frame LSNs are).  Returns 0 on success, negative on error.
 */
int
pgl_walscan_begin(uint64 start, uint64 end, uint32 tli)
{
	MemoryContext oldcxt;

	pgl_walscan_end_scan();

	if (start == 0 || end <= start)
		return -1;

	pglws_tli = (tli == 0) ? 1 : (TimeLineID) tli;
	pglws_end = (XLogRecPtr) end;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	if (!pglws_json_init)
	{
		initStringInfo(&pglws_json);
		pglws_json_init = true;
	}
	pglws_reader = XLogReaderAllocate(wal_segment_size, NULL,
									  XL_ROUTINE(.page_read = pglws_read_page,
												 .segment_open = NULL,
												 .segment_close = NULL),
									  NULL);
	MemoryContextSwitchTo(oldcxt);

	if (pglws_reader == NULL)
		return -2;

	XLogBeginRead(pglws_reader, (XLogRecPtr) start);
	return 0;
}

/* ---- JSON helpers (all keys/values controlled; no escaping needed) ---- */

static void
pglws_json_u64(StringInfo b, const char *key, uint64 v)
{
	/* u64 as decimal string: JS parses with BigInt() */
	appendStringInfo(b, ",\"%s\":\"" UINT64_FORMAT "\"", key, v);
}

static void
pglws_json_hex(StringInfo b, const char *key, const void *data, size_t len)
{
	appendStringInfo(b, ",\"%s\":\"", key);
	enlargeStringInfo(b, (int) (len * 2 + 1));
	b->len += (int) hex_encode((const char *) data, len, b->data + b->len);
	b->data[b->len] = '\0';
	appendStringInfoChar(b, '"');
}

static void
pglws_json_rel(StringInfo b, const char *key, const RelFileLocator *rl)
{
	appendStringInfo(b, ",\"%s\":[%u,%u,%u]", key,
					 rl->spcOid, rl->dbOid, rl->relNumber);
}

/* Decoded payloads for the eager set (§6.3). */
static void
pglws_decode_xact(StringInfo b, XLogReaderState *r, uint8 info)
{
	uint8		op = info & XLOG_XACT_OPMASK;

	if (op == XLOG_XACT_COMMIT || op == XLOG_XACT_COMMIT_PREPARED)
	{
		xl_xact_commit *xlrec = (xl_xact_commit *) XLogRecGetData(r);
		xl_xact_parsed_commit parsed;

		ParseCommitRecord(XLogRecGetInfo(r), xlrec, &parsed);
		appendStringInfoString(b, ",\"kind\":\"commit\"");
		if (op == XLOG_XACT_COMMIT_PREPARED)
			appendStringInfo(b, ",\"twophase_xid\":%u", parsed.twophase_xid);
		appendStringInfo(b, ",\"dbId\":%u,\"tsId\":%u",
						 parsed.dbId, parsed.tsId);
		appendStringInfo(b, ",\"relcacheInitFileInval\":%s",
						 XactCompletionRelcacheInitFileInval(parsed.xinfo)
						 ? "true" : "false");
		appendStringInfoString(b, ",\"subxids\":[");
		for (int i = 0; i < parsed.nsubxacts; i++)
			appendStringInfo(b, "%s%u", i ? "," : "", parsed.subxacts[i]);
		appendStringInfoChar(b, ']');
		appendStringInfoString(b, ",\"drops\":[");
		for (int i = 0; i < parsed.nrels; i++)
			appendStringInfo(b, "%s[%u,%u,%u]", i ? "," : "",
							 parsed.xlocators[i].spcOid,
							 parsed.xlocators[i].dbOid,
							 parsed.xlocators[i].relNumber);
		appendStringInfoChar(b, ']');
		appendStringInfo(b, ",\"nmsgs\":%d", parsed.nmsgs);
		if (parsed.nmsgs > 0)
			pglws_json_hex(b, "invals", parsed.msgs,
						   parsed.nmsgs * sizeof(SharedInvalidationMessage));
	}
	else if (op == XLOG_XACT_ABORT || op == XLOG_XACT_ABORT_PREPARED)
	{
		xl_xact_abort *xlrec = (xl_xact_abort *) XLogRecGetData(r);
		xl_xact_parsed_abort parsed;

		ParseAbortRecord(XLogRecGetInfo(r), xlrec, &parsed);
		appendStringInfoString(b, ",\"kind\":\"abort\"");
		if (op == XLOG_XACT_ABORT_PREPARED)
			appendStringInfo(b, ",\"twophase_xid\":%u", parsed.twophase_xid);
		appendStringInfoString(b, ",\"subxids\":[");
		for (int i = 0; i < parsed.nsubxacts; i++)
			appendStringInfo(b, "%s%u", i ? "," : "", parsed.subxacts[i]);
		appendStringInfoChar(b, ']');
		appendStringInfoString(b, ",\"drops\":[");
		for (int i = 0; i < parsed.nrels; i++)
			appendStringInfo(b, "%s[%u,%u,%u]", i ? "," : "",
							 parsed.xlocators[i].spcOid,
							 parsed.xlocators[i].dbOid,
							 parsed.xlocators[i].relNumber);
		appendStringInfoChar(b, ']');
	}
	else if (op == XLOG_XACT_INVALIDATIONS)
	{
		xl_xact_invals *xlrec = (xl_xact_invals *) XLogRecGetData(r);

		appendStringInfoString(b, ",\"kind\":\"invalidations\"");
		appendStringInfo(b, ",\"nmsgs\":%d", xlrec->nmsgs);
		if (xlrec->nmsgs > 0)
			pglws_json_hex(b, "invals", xlrec->msgs,
						   xlrec->nmsgs * sizeof(SharedInvalidationMessage));
	}
}

static void
pglws_decode_eager(StringInfo b, XLogReaderState *r)
{
	RmgrId		rmid = XLogRecGetRmid(r);
	uint8		info = XLogRecGetInfo(r) & ~XLR_INFO_MASK;

	switch (rmid)
	{
		case RM_XACT_ID:
			pglws_decode_xact(b, r, info);
			break;

		case RM_XLOG_ID:
			if (info == XLOG_NEXTOID)
			{
				Oid			nextOid;

				memcpy(&nextOid, XLogRecGetData(r), sizeof(Oid));
				appendStringInfo(b, ",\"kind\":\"nextoid\",\"nextOid\":%u",
								 nextOid);
			}
			else if (info == XLOG_CHECKPOINT_SHUTDOWN ||
					 info == XLOG_CHECKPOINT_ONLINE)
			{
				CheckPoint	cp;

				memcpy(&cp, XLogRecGetData(r), sizeof(CheckPoint));
				appendStringInfo(b, ",\"kind\":\"checkpoint\",\"shutdown\":%s",
								 info == XLOG_CHECKPOINT_SHUTDOWN
								 ? "true" : "false");
				pglws_json_u64(b, "redo", cp.redo);
				pglws_json_u64(b, "nextXid",
							   U64FromFullTransactionId(cp.nextXid));
				appendStringInfo(b,
								 ",\"nextOid\":%u,\"nextMulti\":%u,\"nextMultiOffset\":%u",
								 cp.nextOid, cp.nextMulti,
								 cp.nextMultiOffset);
			}
			else if (info == XLOG_FPI || info == XLOG_FPI_FOR_HINT)
				appendStringInfoString(b, ",\"kind\":\"fpi\"");
			else if (info == XLOG_PARAMETER_CHANGE)
				appendStringInfoString(b, ",\"kind\":\"parameter_change\"");
			break;

		case RM_HEAP_ID:
			if ((info & XLOG_HEAP_OPMASK) == XLOG_HEAP_INPLACE)
			{
				xl_heap_inplace *xlrec =
					(xl_heap_inplace *) XLogRecGetData(r);

				appendStringInfoString(b, ",\"kind\":\"heap_inplace\"");
				appendStringInfo(b, ",\"dbId\":%u,\"tsId\":%u",
								 xlrec->dbId, xlrec->tsId);
				appendStringInfo(b, ",\"relcacheInitFileInval\":%s",
								 xlrec->relcacheInitFileInval
								 ? "true" : "false");
				appendStringInfo(b, ",\"nmsgs\":%d", xlrec->nmsgs);
				if (xlrec->nmsgs > 0)
					pglws_json_hex(b, "invals", xlrec->msgs,
								   xlrec->nmsgs * sizeof(SharedInvalidationMessage));
			}
			break;

		case RM_MULTIXACT_ID:
			if (info == XLOG_MULTIXACT_CREATE_ID)
			{
				xl_multixact_create *xlrec =
					(xl_multixact_create *) XLogRecGetData(r);

				appendStringInfo(b,
								 ",\"kind\":\"multixact_create\",\"mid\":%u,\"moff\":%u,\"nmembers\":%d",
								 xlrec->mid, xlrec->moff, xlrec->nmembers);
				appendStringInfoString(b, ",\"members\":[");
				for (int i = 0; i < xlrec->nmembers; i++)
					appendStringInfo(b, "%s[%u,%d]", i ? "," : "",
									 xlrec->members[i].xid,
									 (int) xlrec->members[i].status);
				appendStringInfoChar(b, ']');
				pglws_json_hex(b, "members_raw", xlrec->members,
							   xlrec->nmembers * sizeof(MultiXactMember));
			}
			else if (info == XLOG_MULTIXACT_ZERO_OFF_PAGE ||
					 info == XLOG_MULTIXACT_ZERO_MEM_PAGE)
			{
				int64		pageno;

				memcpy(&pageno, XLogRecGetData(r), sizeof(pageno));
				appendStringInfo(b, ",\"kind\":\"%s\"",
								 info == XLOG_MULTIXACT_ZERO_OFF_PAGE
								 ? "multixact_zero_off_page"
								 : "multixact_zero_mem_page");
				pglws_json_u64(b, "pageno", (uint64) pageno);
			}
			else if (info == XLOG_MULTIXACT_TRUNCATE_ID)
				appendStringInfoString(b, ",\"kind\":\"multixact_truncate\"");
			break;

		case RM_CLOG_ID:
			if (info == CLOG_ZEROPAGE)
			{
				int64		pageno;

				memcpy(&pageno, XLogRecGetData(r), sizeof(pageno));
				appendStringInfoString(b, ",\"kind\":\"clog_zero_page\"");
				pglws_json_u64(b, "pageno", (uint64) pageno);
			}
			else if (info == CLOG_TRUNCATE)
				appendStringInfoString(b, ",\"kind\":\"clog_truncate\"");
			break;

		case RM_SEQ_ID:
			if (info == XLOG_SEQ_LOG)
			{
				xl_seq_rec *xlrec = (xl_seq_rec *) XLogRecGetData(r);

				appendStringInfoString(b, ",\"kind\":\"seq_log\"");
				pglws_json_rel(b, "seqRel", &xlrec->locator);
			}
			break;

		case RM_SMGR_ID:
			if (info == XLOG_SMGR_CREATE)
			{
				xl_smgr_create *xlrec = (xl_smgr_create *) XLogRecGetData(r);

				appendStringInfo(b, ",\"kind\":\"smgr_create\",\"fork\":%d",
								 (int) xlrec->forkNum);
				pglws_json_rel(b, "rel", &xlrec->rlocator);
			}
			else if (info == XLOG_SMGR_TRUNCATE)
			{
				xl_smgr_truncate *xlrec =
					(xl_smgr_truncate *) XLogRecGetData(r);

				appendStringInfo(b,
								 ",\"kind\":\"smgr_truncate\",\"blkno\":%u,\"flags\":%d",
								 xlrec->blkno, xlrec->flags);
				pglws_json_rel(b, "rel", &xlrec->rlocator);
			}
			break;

		case RM_DBASE_ID:
			if (info == XLOG_DBASE_CREATE_FILE_COPY)
			{
				xl_dbase_create_file_copy_rec *xlrec =
					(xl_dbase_create_file_copy_rec *) XLogRecGetData(r);

				appendStringInfo(b,
								 ",\"kind\":\"dbase_create\",\"dbId\":%u,\"tsId\":%u,\"srcDbId\":%u,\"srcTsId\":%u",
								 xlrec->db_id, xlrec->tablespace_id,
								 xlrec->src_db_id, xlrec->src_tablespace_id);
			}
			else if (info == XLOG_DBASE_CREATE_WAL_LOG)
			{
				xl_dbase_create_wal_log_rec *xlrec =
					(xl_dbase_create_wal_log_rec *) XLogRecGetData(r);

				appendStringInfo(b,
								 ",\"kind\":\"dbase_create\",\"dbId\":%u,\"tsId\":%u",
								 xlrec->db_id, xlrec->tablespace_id);
			}
			else if (info == XLOG_DBASE_DROP)
			{
				xl_dbase_drop_rec *xlrec =
					(xl_dbase_drop_rec *) XLogRecGetData(r);

				appendStringInfo(b, ",\"kind\":\"dbase_drop\",\"dbId\":%u",
								 xlrec->db_id);
			}
			break;

		default:
			break;
	}
}

/*
 * pgl_walscan_next
 *		Return the next classified record as a NUL-terminated JSON string
 *		(pointer into WASM memory, valid until the next call), or 0 when
 *		the range is exhausted or on error (error JSON is returned once,
 *		then 0).
 */
uintptr_t
pgl_walscan_next(void)
{
	XLogRecord *record;
	char	   *errormsg = NULL;
	StringInfo	b;

	if (pglws_reader == NULL)
		return 0;

	/* Done once the last returned record reached or passed the end. */
	if (pglws_reader->EndRecPtr >= pglws_end)
		return 0;

	record = XLogReadRecord(pglws_reader, &errormsg);

	b = &pglws_json;
	resetStringInfo(b);

	if (record == NULL)
	{
		appendStringInfoString(b, "{\"error\":");
		escape_json(b, errormsg ? errormsg : "read failed");
		appendStringInfoChar(b, '}');
		pgl_walscan_end_scan();
		/* json buffer survives end_scan (TopMemoryContext) */
		return (uintptr_t) pglws_json.data;
	}

	/* A record that starts before end but ends past it is out of range. */
	if (pglws_reader->ReadRecPtr >= pglws_end)
	{
		pgl_walscan_end_scan();
		return 0;
	}

	appendStringInfoChar(b, '{');
	appendStringInfo(b, "\"rmid\":%d,\"info\":%d,\"xid\":%u",
					 (int) XLogRecGetRmid(pglws_reader),
					 (int) (XLogRecGetInfo(pglws_reader) & ~XLR_INFO_MASK),
					 XLogRecGetXid(pglws_reader));
	pglws_json_u64(b, "lsn", pglws_reader->ReadRecPtr);
	pglws_json_u64(b, "end", pglws_reader->EndRecPtr);

	appendStringInfoString(b, ",\"blocks\":[");
	for (int i = 0; i <= XLogRecMaxBlockId(pglws_reader); i++)
	{
		RelFileLocator rlocator;
		ForkNumber	fork;
		BlockNumber blk;
		DecodedBkpBlock *bkb;

		if (!XLogRecHasBlockRef(pglws_reader, i))
			continue;
		XLogRecGetBlockTagExtended(pglws_reader, i, &rlocator, &fork, &blk,
								   NULL);
		bkb = XLogRecGetBlock(pglws_reader, i);
		appendStringInfo(b,
						 "%s{\"rel\":[%u,%u,%u],\"fork\":%d,\"blk\":%u,\"img\":%s,\"init\":%s}",
						 b->data[b->len - 1] == '[' ? "" : ",",
						 rlocator.spcOid, rlocator.dbOid, rlocator.relNumber,
						 (int) fork, blk,
						 (bkb->has_image) ? "true" : "false",
						 (bkb->flags & BKPBLOCK_WILL_INIT) ? "true" : "false");
	}
	appendStringInfoChar(b, ']');

	pglws_decode_eager(b, pglws_reader);

	appendStringInfoChar(b, '}');
	return (uintptr_t) pglws_json.data;
}

/*
 * pgl_walscan_block_image
 *		Restore the full-page image carried by block_id of the CURRENT
 *		record (the one last returned by pgl_walscan_next) into dst
 *		(BLCKSZ bytes) — decompression and hole re-expansion included
 *		(RestoreBlockImage).  Returns 1 on success, 0 if there is no
 *		image or no current record.  This is what makes has_image blocks
 *		live-appliable: the host writes the restored page to the datadir
 *		and drops the buffer, so on-demand re-read serves post-apply
 *		bytes (design §6.2 follower invariant, M5b live apply v1).
 */
int
pgl_walscan_block_image(int block_id, uintptr_t dst)
{
	if (pglws_reader == NULL || pglws_reader->record == NULL ||
		dst == 0 ||
		block_id < 0 || block_id > XLogRecMaxBlockId(pglws_reader))
		return 0;
	if (!XLogRecHasBlockRef(pglws_reader, block_id) ||
		!XLogRecHasBlockImage(pglws_reader, block_id))
		return 0;
	if (!RestoreBlockImage(pglws_reader, block_id, (char *) dst))
		return 0;
	return 1;
}

/*
 * Single-record redo harness (M5c, design §14.2 "page materialization at
 * LSN").
 *
 * REALITY CHECK RESULT (documented per plan): a fully generic private-page
 * walredo (Neon-style) needs a coaxed buffer manager and deep surgery.  In
 * the PGlite single-backend build the pragmatic variant IS sound: rm_redo
 * runs against the LIVE shared buffers via XLogReadBufferForRedo, which
 * works outside recovery — the buffer read path has no InRecovery gate
 * (only the beyond-EOF extension path asserts, relaxed under
 * pgl_redo_active), hot-standby conflict handling is skipped because
 * standbyState stays STANDBY_DISABLED, and single-session cells have no
 * snapshot to conflict with between transactions.  The one real hazard is
 * FlushBuffer → XLogFlush(pageLSN) PANICking on LSNs ahead of the local
 * flush state, which is why the host MUST advance the WAL insert position
 * (pgl_set_wal_position) past the applied range BEFORE redoing records.
 *
 * The whitelist is the buffer-only rmgrs: their redo routines touch shared
 * buffers / FSM / VM only.  Everything with file, SLRU, or shared-state
 * side effects (xact, smgr, dbase, clog, multixact, relmap, tblspc,
 * sequence — lease-governed, §5.3) stays on the semantic JS pipeline or
 * falls back to recycle.
 */
bool		pgl_redo_active = false;

/*
 * Redo-harness state: some rmgrs (btree, gin, gist, spgist) allocate a
 * private memory context in rm_startup() and switch into it inside
 * rm_redo — without the startup call they dereference NULL.  We start an
 * rmgr lazily on first use (its context parented to TopMemoryContext)
 * and run rm_cleanup for every started rmgr at scan end.  rm_redo itself
 * runs under a dedicated reset-per-record context so stray pallocs never
 * accumulate or land in a caller context.
 */
static bool pglws_rm_started[RM_MAX_ID + 1];
static MemoryContext pglws_redo_ctx = NULL;

static bool
pglws_redo_whitelisted(RmgrId rmid, uint8 info)
{
	switch (rmid)
	{
		case RM_XLOG_ID:
			/* FPI restore only; other xlog records carry no page changes
			 * we should apply this way. */
			return (info == XLOG_FPI || info == XLOG_FPI_FOR_HINT);
		case RM_HEAP2_ID:
			/* Logical-rewrite records write mapping files: not buffer-only. */
			return (info & XLOG_HEAP_OPMASK) != XLOG_HEAP2_REWRITE;
		case RM_SEQ_ID:
			/*
			 * seq_redo is buffer-only (page reinit from the logged tuple).
			 * Applying it does not move allocation AUTHORITY, which stays
			 * with the host's leases/floors (§5.3) — the host re-floors and
			 * re-clamps after an advance.  NOT applying it would leave
			 * sequences created after base as zero-length files.
			 */
			return (info == XLOG_SEQ_LOG);
		case RM_HEAP_ID:
		case RM_BTREE_ID:
		case RM_HASH_ID:
		case RM_GIN_ID:
		case RM_GIST_ID:
		case RM_SPGIST_ID:
		case RM_BRIN_ID:
		case RM_GENERIC_ID:
			return true;
		default:
			return false;
	}
}

/* JS-visible probe: would redo_current handle this rmid/info? */
int
pgl_redo_whitelisted(int rmid, int info)
{
	return pglws_redo_whitelisted((RmgrId) rmid, (uint8) info) ? 1 : 0;
}

int
pgl_walscan_redo_current(void)
{
	RmgrId		rmid;
	uint8		info;
	ResourceOwner owner;
	ResourceOwner oldOwner;
	MemoryContext oldCtx;
	int			rc = 1;

	if (pglws_reader == NULL || pglws_reader->record == NULL)
		return -1;

	rmid = XLogRecGetRmid(pglws_reader);
	info = XLogRecGetInfo(pglws_reader) & ~XLR_INFO_MASK;
	if (!pglws_redo_whitelisted(rmid, info))
		return 0;

	if (pglws_redo_ctx == NULL)
		pglws_redo_ctx = AllocSetContextCreate(TopMemoryContext,
											   "pgl_redo",
											   ALLOCSET_DEFAULT_SIZES);

	/* Lazy rm_startup (its private context parents to TopMemoryContext). */
	if (!pglws_rm_started[rmid])
	{
		oldCtx = MemoryContextSwitchTo(TopMemoryContext);
		if (GetRmgr(rmid).rm_startup != NULL)
			GetRmgr(rmid).rm_startup();
		MemoryContextSwitchTo(oldCtx);
		pglws_rm_started[rmid] = true;
	}

	/*
	 * Run the redo under a private resource owner so buffer pins are
	 * releasable on error (the export runs between transactions, with no
	 * transaction resource owner to clean up after us).
	 */
	owner = ResourceOwnerCreate(NULL, "pgl_redo");
	oldOwner = CurrentResourceOwner;
	CurrentResourceOwner = owner;
	oldCtx = MemoryContextSwitchTo(pglws_redo_ctx);
	pgl_redo_active = true;

	PG_TRY();
	{
		GetRmgr(rmid).rm_redo(pglws_reader);
	}
	PG_CATCH();
	{
		pgl_redo_active = false;
		FlushErrorState();
		LWLockReleaseAll();
		UnlockBuffers();
		rc = -2;
	}
	PG_END_TRY();

	pgl_redo_active = false;
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(pglws_redo_ctx);
	CurrentResourceOwner = owner;
	ResourceOwnerRelease(owner, RESOURCE_RELEASE_BEFORE_LOCKS, false, false);
	ResourceOwnerRelease(owner, RESOURCE_RELEASE_LOCKS, false, false);
	ResourceOwnerRelease(owner, RESOURCE_RELEASE_AFTER_LOCKS, false, false);
	CurrentResourceOwner = oldOwner;
	ResourceOwnerDelete(owner);

	return rc;
}

/*
 * pgl_walscan_end_scan
 *		Release scan resources.  Idempotent.
 */
void
pgl_walscan_end_scan(void)
{
	/* Tear down any rmgrs the redo harness started for this scan. */
	for (int rmid = 0; rmid <= RM_MAX_ID; rmid++)
	{
		if (pglws_rm_started[rmid])
		{
			if (GetRmgr(rmid).rm_cleanup != NULL)
				GetRmgr(rmid).rm_cleanup();
			pglws_rm_started[rmid] = false;
		}
	}

	if (pglws_reader != NULL)
	{
		XLogReaderFree(pglws_reader);
		pglws_reader = NULL;
	}
	if (pglws_fd >= 0)
	{
		close(pglws_fd);
		pglws_fd = -1;
	}
	pglws_segno = 0;
	pglws_end = InvalidXLogRecPtr;
}
