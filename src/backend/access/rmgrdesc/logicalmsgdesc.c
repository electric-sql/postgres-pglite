/*-------------------------------------------------------------------------
 *
 * logicalmsgdesc.c
 *	  rmgr descriptor routines for replication/logical/message.c
 *
 * Portions Copyright (c) 2015-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/logicalmsgdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "replication/message.h"

#ifdef PGLITE_WASM32_WAL
/*
 * Phase 7 runs native frontend test tools against clusters produced by a
 * wasm32 PostgreSQL server.  xl_logical_message is the only built-in WAL
 * record containing Size fields, so its on-disk layout differs between the
 * two pointer widths.  Fence the host test-tool decoder explicitly rather
 * than changing PostgreSQL's production WAL format or server redo path.
 */
typedef struct xl_logical_message_wasm32
{
	Oid			dbId;
	uint8		transactional;
	uint8		padding[3];
	uint32		prefix_size;
	uint32		message_size;
	char		message[FLEXIBLE_ARRAY_MEMBER];
} xl_logical_message_wasm32;

StaticAssertDecl(offsetof(xl_logical_message_wasm32, message) == 16,
				 "unexpected wasm32 logical-message WAL layout");
#endif

void
logicalmsg_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_LOGICAL_MESSAGE)
	{
#ifdef PGLITE_WASM32_WAL
		xl_logical_message_wasm32 *xlrec = (xl_logical_message_wasm32 *) rec;
#else
		xl_logical_message *xlrec = (xl_logical_message *) rec;
#endif
		char	   *prefix = xlrec->message;
		char	   *message = xlrec->message + xlrec->prefix_size;
		char	   *sep = "";

		Assert(prefix[xlrec->prefix_size - 1] == '\0');

		appendStringInfo(buf, "%s, prefix \"%s\"; payload (%zu bytes): ",
						 xlrec->transactional ? "transactional" : "non-transactional",
						 prefix, xlrec->message_size);
		/* Write message payload as a series of hex bytes */
		for (int cnt = 0; cnt < xlrec->message_size; cnt++)
		{
			appendStringInfo(buf, "%s%02X", sep, (unsigned char) message[cnt]);
			sep = " ";
		}
	}
}

const char *
logicalmsg_identify(uint8 info)
{
	if ((info & ~XLR_INFO_MASK) == XLOG_LOGICAL_MESSAGE)
		return "MESSAGE";

	return NULL;
}
