#pragma once

// Minimal compatibility header to build pglite glue (interactive_one.c)
// outside of the wasm toolchains, using Android NDK. We:
// - provide standard types and headers
// - define CMA_MB / CMA_FD defaults if not provided by caller
// - forward declare a few PG types used by pointer only
// - include Postgres server headers for struct/function prototypes

#include <stdio.h>
#include <stdbool.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

#include <stdint.h>


// Forward declarations used by interactive_one.c

// Ensure shm_* feature macros from pg_config.h don't pull in shm_open on Android glue
#ifdef HAVE_SHM_OPEN
#undef HAVE_SHM_OPEN
#endif
#ifdef HAVE_SHM_UNLINK
#undef HAVE_SHM_UNLINK
#endif

// Forward declare proc_exit used in backend C units included by pg_main.c
#ifndef HAVE_PROC_EXIT_DECL
#define HAVE_PROC_EXIT_DECL 1
extern void proc_exit(int code);
#endif

#ifndef HAVE_PORT_FWD
#define HAVE_PORT_FWD 1
typedef struct Port Port;
/* Do NOT typedef ClientSocket here; libpq-be.h defines it as a struct. */
#endif

// Pull in PostgreSQL server headers (installed by `make -C src/include install`)
// so we get definitions for StringInfoData, ereport, etc.
#include "postgres.h"
#include "lib/stringinfo.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "miscadmin.h"
#include "utils/timeout.h"

// WASM glue uses PDEBUG for tracing; on mobile we compile it away

// CMA defaults: let pg_main.c define them; do not override here.

#ifndef PDEBUG
#define PDEBUG(...) do {} while(0)
#endif


// Android API 24+: preadv/pwritev provided by bionic; no fallbacks necessary.



/* Note: On native builds, WASM export_name attributes are ignored.
   Provide any symbol name shims in a separate TU (pgl_mobile_shims.c)
   rather than using GCC alias attributes here. */
