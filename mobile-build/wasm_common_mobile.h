#pragma once
// Provide macros compatible with wasm_common.h for file-mode paths on mobile
#ifndef PGS_ILOCK
#define PGS_ILOCK "/tmp/pglite/base/.s.PGSQL.5432.lock.in"
#endif
#ifndef PGS_IN
#define PGS_IN    "/tmp/pglite/base/.s.PGSQL.5432.in"
#endif
#ifndef PGS_OLOCK
#define PGS_OLOCK "/tmp/pglite/base/.s.PGSQL.5432.lock.out"
#endif
#ifndef PGS_OUT
#define PGS_OUT   "/tmp/pglite/base/.s.PGSQL.5432.out"
#endif


// Defaults mirroring wasm environment, used by pg_main.c and interactive_one.c
#ifndef WASM_PREFIX
#define WASM_PREFIX "/tmp/pglite"
#endif
#ifndef WASM_USERNAME
#define WASM_USERNAME "postgres"
#endif
#ifndef WASM_PGOPTS
#define WASM_PGOPTS ""
#endif
#ifndef CMA_MB
#define CMA_MB 12
#endif


// Mobile builds are not WASM; do not inject __wasi__/__EMSCRIPTEN__ here.
// Keep only path/CMA constants in this header.

// Emscripten-only macros: define as no-ops in case code references them
#ifndef EM_ASM
#define EM_ASM(...) ((void)0)
#endif
#ifndef EMSCRIPTEN_KEEPALIVE
#define EMSCRIPTEN_KEEPALIVE
#endif

