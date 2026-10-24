#ifndef I_WASM
#define I_WASM

// #include "port/wasm_common.h"

#define WAIT_USE_POLL 1

#define HAVE_LINUX_EIDRM_BUG

#define PLATFORM_DEFAULT_WAL_SYNC_METHOD	WAL_SYNC_METHOD_FDATASYNC

#define COPY_OFF
#define PGDLLIMPORT
// c.h turns this into "#define inline", which breaks C++ extensions including postgres.h (ie pgRouting):
// inline functions of the C++ headers included afterwards become strong symbols -> duplicate symbols at link time
#ifndef __cplusplus
#define PG_FORCE_DISABLE_INLINE
#endif

#endif // I_WASM