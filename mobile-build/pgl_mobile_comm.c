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
#ifdef __ANDROID__
#include <android/log.h>
#define MLOGI(...) __android_log_print(ANDROID_LOG_INFO, "PGLiteMobileComm", __VA_ARGS__)
#else
#define MLOGI(...)
#endif

// Mobile CMA shim
#include "sdk_port-mobile.h"

/* External variables defined in pqcomm.c */
extern volatile int pgl_mobile_cma_wsize;

static StringInfoData mobileSendBuf;
static bool mobileCommInited = false;
static int prev_req_len = -1;      /* last seen cma_rsize for request */
static int out_published = 0;      /* cumulative bytes published for current request */

static void mobile_comm_init_if_needed(void)
{
    if (!mobileCommInited)
    {
        initStringInfo(&mobileSendBuf);
        mobileCommInited = true;
    }
}

static void mobile_comm_reset(void)
{
    mobile_comm_init_if_needed();
    resetStringInfo(&mobileSendBuf);
}

static int mobile_flush(void)
{
    mobile_comm_init_if_needed();
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
            int reqLen = cma_rsize;
            if (prev_req_len != reqLen && reqLen > 0) {
                /* New request began (only reset when reqLen > 0) */
                out_published = 0;
                prev_req_len = reqLen;
                // MLOGI("mobile_flush: new request, reqLen=%d", reqLen);
            } else if (prev_req_len != reqLen) {
                /* Request finished (reqLen == 0), just update tracking */
                prev_req_len = reqLen;
                // MLOGI("mobile_flush: request finished, reqLen=%d", reqLen);
            }
            int off = reqLen + 2 + out_published; /* append after previous chunks */
            if (off < 0) off = 0;
            if (off > cap) off = cap;
            int copyLen = (off + n <= cap) ? n : (cap - off);
            if (copyLen > 0) memcpy(dst + off, mobileSendBuf.data, (size_t)copyLen);
            // publish cumulative length for this request
            out_published += copyLen;
            channel = 1;
            pgl_mobile_cma_wsize = out_published;
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_INFO, "PGLiteMobileComm", "flush: set pgl_mobile_cma_wsize=%d, addr=%p", pgl_mobile_cma_wsize, (void*)&pgl_mobile_cma_wsize);
#endif
            // MLOGI("flush: published chunk=%d cum=%d at off=%d (reqLen=%d)", copyLen, out_published, off, reqLen);
        }
        else {
            MLOGI("flush: mobileSendBuf.len=%d but cap insufficient (cap=%d, reqLen=%d)", mobileSendBuf.len, cap, cma_rsize);
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
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "PGLiteMobileComm", "mobile_putmessage: msgtype='%c' len=%zu", msgtype, len);
#endif

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
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "PGLiteMobileComm", "pgl_install_mobile_comm: Installing mobile comm methods");
#endif
    static const PQcommMethods MobileMethods = {
        .comm_reset = mobile_comm_reset,
        .flush = mobile_flush,
        .flush_if_writable = mobile_flush_if_writable,
        .is_send_pending = mobile_is_send_pending,
        .putmessage = mobile_putmessage,
        .putmessage_noblock = mobile_putmessage_noblock
    };
    PqCommMethods = &MobileMethods;
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "PGLiteMobileComm", "pgl_install_mobile_comm: PqCommMethods set to %p", (void*)PqCommMethods);
#endif
    
    /* Ensure CMA buffer is initialized and external variables are set */
    get_buffer_size(0);  /* This will trigger ensure_buf() */
}

