#include "sdk_port-mobile.h"
#include <stdlib.h>
#include <string.h>
#include "../pglite-wasm/pgl_os.h"

/* When included in PostgreSQL build, these variables are defined in postgres.c */
#ifdef POSTGRES_H
extern volatile int cma_rsize;  /* defined in postgres.c */
#endif

#ifndef POSTGRES_H
/* Standalone mobile build - define variables here */
volatile int cma_rsize = 0;
#endif

/* External variables defined in pqcomm.c, set by mobile SDK */
extern volatile int pgl_mobile_cma_wsize;
extern int original_request_size;  /* Defined in pgl_mobile_comm.c */

/* Mobile: Single definition point for these globals (used by interactive_one.c) */
volatile int channel = 0;
volatile bool is_wire = false;  /* Default to REPL mode for bootstrap */
volatile bool is_repl = true;   /* Start in REPL mode */

// Single channel buffer for now
#ifndef CMA_MB
#define CMA_MB 12
#endif
#ifndef CMA_FD
#define CMA_FD 1
#endif

static uint8_t* g_buf = NULL;
static int g_cap = 0;

/* External variables defined in pqcomm.c, set by mobile SDK */
extern void* pgl_mobile_cma_buffer_addr;
extern int pgl_mobile_cma_buffer_size;

static void ensure_buf() {
  if (!g_buf) {
    g_cap = (CMA_MB * 1024 * 1024) / CMA_FD;
    void* p = NULL;
    if (posix_memalign(&p, 16, (size_t)g_cap + 2) != 0) {
      p = NULL;
    }
    g_buf = (uint8_t*)p;
    if (g_buf) {
      memset(g_buf, 0, (size_t)g_cap + 2);
      /* Set external variables for PostgreSQL to access this buffer */
      pgl_mobile_cma_buffer_addr = g_buf + 1;  /* Match WASM offset */
      pgl_mobile_cma_buffer_size = g_cap;
      PGL_LOG_INFO("ensure_buf: set pgl_mobile_cma_buffer_addr=%p, size=%d, g_buf=%p", 
                         pgl_mobile_cma_buffer_addr, pgl_mobile_cma_buffer_size, (void*)g_buf);
    }
  }
}

int get_buffer_size(int fd) {
  (void)fd;
  ensure_buf();
  return g_cap;
}

intptr_t get_buffer_addr(int fd) {
  (void)fd;
  ensure_buf();
  // Return native pointer to (buf + 1) to match WASM IO semantics
  return (intptr_t)(g_buf + 1);
}

int interactive_read(void) {
  PGL_LOG_INFO("interactive_read: pgl_mobile_cma_wsize=%d, addr=%p", pgl_mobile_cma_wsize, (void*)&pgl_mobile_cma_wsize);
  return pgl_mobile_cma_wsize;
}

void use_wire(int state) {
  is_wire = (state > 0);
  extern volatile bool is_repl;
  is_repl = !is_wire;
  
  /* When switching to REPL mode, clear request size to avoid confusion */
  if (!is_wire) {
    original_request_size = 0;
    pgl_mobile_cma_wsize = 0;
    PGL_LOG_INFO("use_wire: switched to REPL mode, cleared request sizes");
  }
}

