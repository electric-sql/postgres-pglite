#ifndef I_WASM
#define I_WASM

// #include "port/wasm_common.h"

#define WAIT_USE_POLL 1

#define HAVE_LINUX_EIDRM_BUG

#define PLATFORM_DEFAULT_WAL_SYNC_METHOD	WAL_SYNC_METHOD_FDATASYNC

#define COPY_OFF
#define PGDLLIMPORT
#define PG_FORCE_DISABLE_INLINE

#endif // I_WASM