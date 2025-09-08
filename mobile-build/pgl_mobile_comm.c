#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include "postgres.h"
#include "miscadmin.h"
#include "lib/stringinfo.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqcomm.h"
#include "utils/guc.h"
#include "../pglite-wasm/pgl_os.h"

// Mobile CMA shim
#include "sdk_port-mobile.h"

/* External variables defined in pqcomm.c */
extern volatile int pgl_mobile_cma_wsize;

static StringInfoData mobileSendBuf;
static bool mobileCommInited = false;
int original_request_size = 0;  /* Saved original request size for offset calculation (non-static for external access) */

static void mobile_comm_init_if_needed(void)
{
    if (!mobileCommInited)
    {
        initStringInfo(&mobileSendBuf);
        mobileCommInited = true;

        PGL_LOG_INFO("mobile_comm_init_if_needed: initialized mobileSendBuf");
    }
}

static void mobile_comm_reset(void)
{
    mobile_comm_init_if_needed();
    resetStringInfo(&mobileSendBuf);
    original_request_size = 0;  /* Reset for new connection */
    
    /* Clear receive buffer state to prevent infinite retry loops */
    extern void pq_reset_buffer_state(void);
    pq_reset_buffer_state();
    PGL_LOG_INFO("mobile_comm_reset: reset receive buffer state (PqRecvPointer=0, PqRecvLength=0)");
    
    /* Clear the CMA buffer to prevent bootstrap data from leaking into queries */
    char *buf = (char*)(intptr_t)get_buffer_addr(1);
    if (buf && get_buffer_size(1) > 0) {
        memset(buf, 0, 256);  /* Clear first 256 bytes */
        PGL_LOG_INFO("mobile_comm_reset: cleared buffer to prevent bootstrap data leakage");
    }
}

static int mobile_flush(void)
{
    mobile_comm_init_if_needed();
    
    /* Bootstrap/single-user queries should never reach here if REPL mode is working correctly */
    extern volatile int cma_rsize;
    if (original_request_size == 0 && cma_rsize == 0) {
        PGL_LOG_WARN("mobile_flush: called without request size set, likely bootstrap query in wire mode");
        /* Don't write anything to avoid buffer corruption */
        resetStringInfo(&mobileSendBuf);
        return 0;
    }
    
    // MLOGI("mobile_flush: mobileSendBuf.len=%d", mobileSendBuf.len);
    if (mobileSendBuf.len > 0)
    {
        // Copy to CMA out buffer (fd 1)
        int cap = get_buffer_size(1);
        int n   = mobileSendBuf.len < cap ? mobileSendBuf.len : cap;
        // MLOGI("mobile_flush: cap=%d n=%d", cap, n);
        if (n > 0)
        {
            char *dst = (char*)(intptr_t)get_buffer_addr(1);
            
            /* Clear separator region if this is a new query (pgl_mobile_cma_wsize == 0) */
            if (pgl_mobile_cma_wsize == 0 && original_request_size > 0) {
                /* Clear the 2-byte separator region that sits between input and output */
                memset(dst + original_request_size, 0, 2);
                PGL_LOG_INFO("mobile_flush: cleared separator region at offset %d", original_request_size);
            }

            /* Calculate offset using original request size (matches WASM behavior) */
            int off = (original_request_size > 0) ? (original_request_size + 2 + pgl_mobile_cma_wsize) : pgl_mobile_cma_wsize;
            if (off < 0) off = 0;
            if (off > cap) off = cap;

            /* Validate that we won't write beyond buffer boundaries */
            int copyLen = (off + n <= cap) ? n : (cap - off);
            if (copyLen > 0) {
                memcpy(dst + off, mobileSendBuf.data, (size_t)copyLen);
                PGL_LOG_INFO("mobile_flush: copied %d bytes to offset %d (original_req_size=%d, current_wsize=%d)",
                           copyLen, off, original_request_size, pgl_mobile_cma_wsize);
            }

            /* Accumulate response size directly in pgl_mobile_cma_wsize */
            pgl_mobile_cma_wsize += copyLen;
            channel = 1;

            PGL_LOG_INFO("flush: updated pgl_mobile_cma_wsize=%d, addr=%p", pgl_mobile_cma_wsize, (void*)&pgl_mobile_cma_wsize);
            // MLOGI("flush: published chunk=%d cum=%d at off=%d (reqLen=%d)", copyLen, out_published, off, reqLen);
        }
        else {
            PGL_LOG_INFO("flush: mobileSendBuf.len=%d but cap insufficient (cap=%d, original_req_size=%d)", mobileSendBuf.len, cap, original_request_size);
        }
        resetStringInfo(&mobileSendBuf);
    }
    else {
        // MLOGI("flush: no pending bytes");
    }
    return 0;
}

static int mobile_flush_if_writable(void)
{
    // In-process, always writable; just delegate
    return mobile_flush();
}

static bool mobile_is_send_pending(void)
{
    mobile_comm_init_if_needed();
    return mobileSendBuf.len > 0;
}

static int mobile_putmessage(char msgtype, const char *s, size_t len)
{
    mobile_comm_init_if_needed();
    PGL_LOG_INFO("mobile_putmessage: msgtype='%c' len=%zu", msgtype, len);

    // For DataRow, parse first couple of field lengths to verify payload integrity
    if (msgtype == 'D' && s != NULL && len >= 2) {
        const unsigned char* p = (const unsigned char*)s;
        // fieldCount is int16 big-endian
        uint16_t fc = (uint16_t)((p[0] << 8) | p[1]);
        PGL_LOG_INFO("mobile_putmessage:   DataRow fieldCount=%u", (unsigned)fc);
        p += 2;
        size_t remaining = len - 2;
        for (unsigned i = 0; i < fc && i < 3 && remaining >= 4; i++) {
            uint32_t flen = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
            PGL_LOG_INFO("mobile_putmessage:     field[%u] len=%u", i, (unsigned)flen);
            p += 4;
            if ((int32_t)flen >= 0) {
                if (remaining < 4 + flen) {
                    PGL_LOG_WARN("mobile_putmessage:     field[%u] payload would exceed message: flen=%u remainingAfterLen=%zu", i, (unsigned)flen, remaining - 4);
                    break;
                }
                // Optional: dump a few bytes of payload
                size_t pl = flen < 8 ? flen : 8;
                char hex[3*8+1];
                size_t pos = 0;
                for (size_t k = 0; k < pl; k++) { pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", p[k]); }
                if (pl > 0) hex[pos ? pos-1 : 0] = '\0';
                PGL_LOG_INFO("mobile_putmessage:       payload[0..%zu)=%s", pl, pl ? hex : "");
                p += flen;
                remaining -= (4 + flen);
            } else {
                remaining -= 4;
            }
        }
    }

    // Format: type + length (int32 network) + payload
    uint32 n32 = htonl((uint32)(len + 4));
    appendStringInfoCharMacro(&mobileSendBuf, msgtype);
    appendBinaryStringInfo(&mobileSendBuf, (const char *)&n32, 4);
    if (len > 0 && s)
        appendBinaryStringInfo(&mobileSendBuf, s, (int)len);
    /* Log notice/error messages content for debugging */
    if (msgtype == 'N' || msgtype == 'E') {
        if (len > 0 && s) {
            /* Log first 200 characters of the message */
            char preview[201];
            size_t preview_len = len < 200 ? len : 200;
            memcpy(preview, s, preview_len);
            preview[preview_len] = '\0';

            /* Also log the raw bytes for debugging */
            char hex_preview[41];
            size_t hex_len = len < 20 ? len : 20;
            for (size_t i = 0; i < hex_len; i++) {
                snprintf(hex_preview + i*2, 3, "%02x", (unsigned char)s[i]);
            }
            hex_preview[hex_len*2] = '\0';

            // MLOGI("putmessage: type=%c len=%zu total=%d content='%s' hex=%s", msgtype, len, mobileSendBuf.len, preview, hex_preview);
        } else {
            // MLOGI("putmessage: type=%c len=%zu total=%d (no content)", msgtype, len, mobileSendBuf.len);
        }
    } else if (msgtype == 'T' || msgtype == 'D' || msgtype == 'C' || msgtype == 'Z') {
        /* Log important message types for query results */
        // MLOGI("putmessage: type=%c (%s) len=%zu total=%d", msgtype,
        //       msgtype == 'T' ? "rowDescription" :
        //       msgtype == 'D' ? "dataRow" :
        //       msgtype == 'C' ? "commandComplete" :
        //       msgtype == 'Z' ? "readyForQuery" : "unknown",
        //       len, mobileSendBuf.len);
    } else {
        // MLOGI("putmessage: type=%c len=%zu total=%d", msgtype, len, mobileSendBuf.len);
    }
    return 0;
}

static void mobile_putmessage_noblock(char msgtype, const char *s, size_t len)
{
    (void)mobile_putmessage(msgtype, s, len);
}

// Hook to install our methods
void pgl_install_mobile_comm(void)
{
    PGL_LOG_INFO("pgl_install_mobile_comm: Installing mobile comm methods");
    static const PQcommMethods MobileMethods = {
        .comm_reset = mobile_comm_reset,
        .flush = mobile_flush,
        .flush_if_writable = mobile_flush_if_writable,
        .is_send_pending = mobile_is_send_pending,
        .putmessage = mobile_putmessage,
        .putmessage_noblock = mobile_putmessage_noblock
    };
    PqCommMethods = &MobileMethods;
    PGL_LOG_INFO("pgl_install_mobile_comm: PqCommMethods set to %p", (void*)PqCommMethods);

    /* Ensure CMA buffer is initialized and external variables are set */
    get_buffer_size(0);  /* This will trigger ensure_buf() */
}

