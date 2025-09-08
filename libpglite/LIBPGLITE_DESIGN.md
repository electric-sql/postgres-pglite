## libpglite host-callback design (draft)

### Overview

Objective: make PGlite embeddable uniformly across environments (mobile, WASM, native) via a stable, versioned C ABI “host ops” surface. The host (wrapper) provides callbacks for transport (wire) and logging, plus optional filesystem mediation. libpglite uses only these callbacks; it avoids platform-specific globals, ad-hoc shared-memory layouts, and deep #ifdefs.

Benefits:

- One API surface for all hosts (React Native, web/WASM, desktop CLI)
- Clear ownership and lifetime rules for buffers
- Stable evolution via abi_version and optional capabilities
- Higher reliability: eliminates brittle offset math and global state in core paths

---

## Proposed Host Ops ABI

Host registers a versioned vtable at init. libpglite never reaches to platform globals; all host interaction goes through ops. The transport is byte-stream oriented; zero-copy is an optimization the host can choose.

Minimal surface (coarse-grained for performance, especially WASM):

- Transport
  - reserve_response(min, out_ptr, out_cap): reserve a contiguous writable window for a full message frame
  - commit_response(n): commit n bytes to the transport
  - flush: finalize a batch and make committed bytes visible
- Logging
  - log(level, msg)
- Versioning
  - abi_version (strict check at init)

Sketch:

```c
typedef struct {
  uint32_t abi_version;
  void*    host_ctx;

  // transport (zero-copy write)
  int  (*reserve_response)(void* ctx, size_t min, uint8_t** out_ptr, size_t* out_cap);
  int  (*commit_response)(void* ctx, size_t n);
  int  (*flush)(void* ctx);

  // logging
  void (*log)(void* ctx, int level, const char* msg);
} PGLiteHostOps;
```

libpglite entry points:

```c
typedef struct PGLiteInstance PGLiteInstance;

int  pglite_init(const PGLiteHostOps*, const PGLiteOptions*, PGLiteInstance** out);
int  pglite_exec_protocol(PGLiteInstance*, const uint8_t* req, size_t req_len);
void pglite_close(PGLiteInstance*);
```

Notes:

- Ownership: host documents lifetime (req/resp buffers). For zero-copy, host ensures buffers remain valid until libpglite signals done (flush/return).
- Single-threaded model: hosts do not consume output during exec; reserve_response must never block for space. The transport maintains a growable append-only buffer per instance.
- Callbacks must be non-throwing and must not longjmp; libpglite wraps callbacks and converts failures to error codes.

---

## Performance and data movement

Goals:

- Minimize WASM/host boundary crossings (1–few calls per command/batch)
- Avoid superfluous copies; enable zero-copy where safe

Principles:

- Batching: libpq already buffers writes; the PQcomm bridge should minimize crossings and call flush sparingly.
- Input: read vtable can return a contiguous view of request bytes (pointer/length) to avoid extra copies; copying is acceptable when required by the platform.
- Output: write via reserve/commit into a host-provided transport buffer. Header (1+4) written contiguously; payload may be chunked across commits with strict ordering.
- Parsing stays on the host: the host parses protocol frames (including notices and NOTIFY) from the same byte stream; no per-frame callbacks in the bridge.

Zero-copy API:

- Scope: zero-copy applies to the transport buffer. Many messages are constructed first into a StringInfo (PG convention) before pq_putmessage; transport avoids extra copies beyond that.

- reserve\*response(size_t min, uint8_t\** ptr, size*t\* cap)
- commit_response(size_t n)

### Message framing with reserve/commit

- For each pq_putmessage(msgtype, payload, len):

  1. Write header contiguously: reserve_response(5, &ptr, &cap); write [msgtype][pg_hton32(len+4)]; commit_response(5)
  2. Write payload in one or more commits: while remaining > 0, reserve_response(min_chunk, &ptr, &cap); n = min(cap, remaining); memcpy(ptr, payload+offset, n); commit_response(n)
  3. Defer flushes; the bridge coalesces until an explicit pq_flush or size threshold

- Non-blocking semantics: reserve_response must not block for space; the transport grows internally (or uses a one-frame scratch on failure) to satisfy putmessage(\_noblock).
- Ordering: preserve message order as produced by libpq.
- Atomicity: header is atomic; payload may be chunked across commits with strict ordering.

#### Edge cases and fallbacks

- Huge frames: If total exceeds any single window the host can provide, the host should either enlarge its window or we temporarily allocate a scratch buffer for this frame, fill it once, then commit into a larger host window when available (one extra copy for this case only).
- Capacity policy: Hosts should size windows at least as large as PQ_SEND_BUFFER_SIZE and ideally larger to avoid fragmentation for common messages.
- Read side: If the host cannot present a contiguous span for a request, the read vtable may fill PqRecvBuffer (one copy) before pq_startmsgread; behavior remains correct.
- Backpressure: Reserve is allowed to fail transiently; the bridge retries only after the host signals space is available; no tight spin loops.

---

- WASM specifics: flush marks committed bytes; JS reads after the call returns. If memory grew during exec, the host must refresh HEAP views before reading.

## Alternative architectures (for exploration)

1. PQcomm bridge + host-ops (proposed)

- Reuse the frontend/backend wire protocol; the PQcomm bridge streams bytes to/from host ops.

2. Socket FD emulation (fake socket backed by shared memory)

- Emulate a socket Port over a ring buffer. Likely not viable on WASM; adds complexity.

3. SQL engine via SPI (bypass wire protocol)

- Direct SQL execution via SPI/Portal. Faster/simpler for embedded use, but divergent from protocol semantics.

4. Dual-mode execution (protocol + SPI)

- Support both protocol and direct SPI for flexibility, at the cost of more surface/testing.

5. Upstream-style PQcomm read vtable

- Add a read vtable similar to write; cleaner separation but touches pqcomm deeper.

6. Out-of-process worker with IPC ring buffer

- Separate process/worker with shared-memory IPC; isolation gains, much higher complexity.

---

## Clean design (no compatibility constraints)

We explicitly drop compatibility with interactive_one.c and current CMA/linear-memory shims. The intent is a small, principled interface in pqcomm and a library entrypoint that drives a single backend session.

### Core abstractions

- Host I/O vtable (read + write):
  - write: keep existing PQcommMethods for send path
  - read: introduce PQcommReadMethods (name TBD) with hooks for:
    - start_msg (begin a message; provides a contiguous view or a reader)
    - recvbuf (fill/read into PqRecvBuffer or return a span)
    - end_msg
    - getbyte_if_available (optional, fast path)
- lib entrypoints (library surface):
  - pglite_init(host_ops, options) -> instance
  - pglite_exec_protocol(instance, req_bytes) -> out via host_ops (streamed or single buffer)
  - pglite_close(instance)

### Minimal, localized Postgres edits (bounded to pqcomm)

- Add read-side vtable alongside PQcommMethods in libpq.h/pqcomm.c
- Refactor pq_startmsgread/pq_recvbuf/pq_getbyte_if_available to go through the read vtable when installed; default to socket implementation otherwise
- No changes to executor/planner/storage, no touching SPI/Portal APIs; MyProcPort still exists but is not required to be a real socket

### Removal/simplification

### Send-side semantics parity with PQcomm

- putmessage_noblock: must not block. Either the host returns a window that always grows to fit, or the bridge builds the frame in a scratch buffer and defers the reserve/commit until flush (one extra copy only for these calls).
- flush_if_writable and is_send_pending: implement via host transport state. flush_if_writable may behave like flush; is_send_pending checks if committed-but-unflushed bytes exist.
- PqCommBusy: avoid reentrancy; never invoke host callbacks that re-enter libpq while busy.
- No file fallback: the transport has a single sink (host buffer). Remove any “redirect to file” concept.

### Read-side coverage

- getbyte_if_available fast path: optional read vtable hook to return a byte without blocking.
- Startup frames: support SSLRequest/CancelRequest/StartupMessage which are length-prefixed without msgtype; the first read may be a raw startup frame.
- Contiguous span vs fill: if host can’t present a contiguous span, the read vtable fills PqRecvBuffer (one copy) and proceeds.

### Zero-copy corner cases

- Very large frames: prefer host to grow a single window >= frame size; otherwise build once in a scratch buffer and copy into a larger window when available; do not split header/payload across windows.
- Backpressure: avoid tight loops; reserve may fail transiently; retry after host signals space.
- Alignment: host must return buffers aligned to >=4 bytes for length header writes.
- WASM memory growth: host must keep reserved pointers stable until commit; avoid invalidating memory views mid-write.

- Remove interactive_one.c and CMA-specific code paths from the build (WASM/mobile)
- Remove PGL_MOBILE / EMSCRIPTEN read-path conditionals inside pq_startmsgread; the read vtable handles platform differences
- Retain a default socket read implementation for regular builds

### WASM feasibility

- The read/write vtables can be implemented via WASM imports (JS provides the host ops)
- Boundary-crossing minimized by batching and by allowing the read vtable to expose a contiguous span for each message

### Performance

- Write path unchanged in spirit (buffer then flush). We can add optional reserve/commit to allow zero-copy writes into a host-provided region
- Read path can be zero-copy by pointing PqRecvBuffer at host memory or by returning spans from the read vtable; otherwise, one memcpy per batch
- Vtable indirections are negligible vs protocol processing; WASM crossing kept coarse (1–few calls per batch)

### Advantages vs current

- Eliminates fragile global CMA offset conventions
- No wrapper-specific code in backend except a small, well-defined vtable in pqcomm
- Clearer portability: host provides a single, stable I/O surface; core stays the same

---

## Filesystem requirements and host responsibilities

This clarifies what FS operations are needed across phases and what the host must provide. The goal is to keep Postgres internals unchanged while allowing portability from strict POSIX to constrained environments (WASM, RN).

### Phases and required operations

1. Initdb (cluster creation)

- Reads: runtime assets from share/postgresql (SQL scripts, templates)
  - fopen/fread/fclose, stat
- Create directory tree under PGDATA
  - mkdir, possibly mkdir -p equivalent
  - optional symlink for pg_wal when using -X (can be disabled to avoid symlink)
- Create/overwrite small text files
  - open(O_CREAT|O_TRUNC), write, close; chmod-like permissions (can be relaxed)
- Durability (optional, depends on mode)
  - fsync files and directories; fsync parent directories; durable_rename
  - In constrained mode, pass --no-sync and treat fsync as no-op

2. Bootstrap catalog/data (backend single-user creating system catalogs)

- Heavy relation file IO under base/, global/, pg_xact/, etc.
  - open(O_CREAT|O_RDWR), pread/pwrite or read/write at offsets, ftruncate
  - rename/unlink for file lifecycle; directory listing to scan/reset
  - opendir/readdir/closedir, stat/lstat
- Temp files
  - create in pg_temp; unlink on close
- Durability (checkpoint/WAL interactions)
  - fdatasync/fsync for relation segments and WAL, directory fsync for parent dirs
  - durable_rename for WAL segment rotation and other atomic updates

3. Normal operation

- Same as bootstrap plus periodic checkpoints, relfilenode creation, truncation
- Temp files for sorts/hash; deletions
- Optional: tablespaces (pg_tblspc symlinks)

### Default approach: standard C/Posix file APIs

- Prefer the platform toolchain’s libc/Posix file APIs everywhere (open/close/read/write/rename/mkdir, etc.).
- This works on: Linux/macOS/Windows (native ports already exist in PG), Android (bionic), iOS (Darwin), WASM (via Emscripten’s VFS like MEMFS/IDBFS/OPFS), WASI (via WASI-libc preopens).
- Host responsibilities per platform:
  - Provide real paths for PGDATA and runtime assets (share/postgresql)
  - For WASM: mount/initialize Emscripten FS (e.g., IDBFS/OPFS) before starting PG; handle async sync if using IDBFS
  - For mobile (iOS/Android): supply sandbox-safe directories; set permissions loosely if needed
  - Set durability knobs: initdb --no-sync and enableFsync=off when durability is not guaranteed; avoid symlinks (-X) unless supported

### Capability profiles and behavior

- durability=strict (default for native platforms)
  - fsync/fdatasync/fsync_dir work normally; durable_rename honored
  - initdb may perform full sync of PGDATA; wal_sync_method chosen appropriately
- durability=relaxed (for constrained envs like WASM)
  - initdb uses --no-sync; enableFsync=off at runtime (fsync becomes no-op)
  - symlink not required; avoid -X so pg_wal is a real directory
- links=absent (for platforms without symlink support)
  - disallow tablespace symlinks and WAL symlink; enforce via options/flags

### Runtime assets (share/)

- initdb needs read-only access to share/postgresql SQL files and templates
- Host must expose a path or virtual FS mount for these assets; the location is passed via options (e.g., runtimeDir/share)

### Mapping to Postgres calls

- fd.c: pg_fsync, pg_fdatasync, fsync_fname(\_ext), PathNameOpenFile, File\*, PathNameDeleteTemporaryFile, durable_rename
- file_utils.c: fsync_parent_path, durable_rename (frontend)
- initdb.c: mkdir, symlink (optional), fopen/fwrite, stat, recursive sync
- reinit.c: directory scans, unlink/reset flows
- postinit.c: ValidatePgVersion, database path checks

### Suggested approach

- Host provides PGDATA/runtime paths and sets durability options (initdb --no-sync, enableFsync) to match the environment.
- Add capability flags in host ops: can_fsync, has_symlink, atomic_rename to guide behavior.
- Use existing Postgres durability controls (enableFsync GUC, initdb --no-sync) rather than intercepting FS calls.

---

## Concrete change list (high-level)

1. Add PQcommReadMethods in src/include/libpq/libpq.h (or a sibling header):
   - struct with function pointers: startmsg, recvbuf/fill, endmsg, getbyte_if_available
   - global pointer PqCommReadMethods with a default socket-backed implementation
2. Modify src/backend/libpq/pqcomm.c:
   - pq_startmsgread -> calls PqCommReadMethods->startmsg()
   - pq_recvbuf -> calls PqCommReadMethods->recvbuf()
   - pq_getbyte_if_available -> optionally calls vtable fast path
   - maintain existing socket code as the default implementation
3. Provide host-ops to pqcomm bridge (in libpglite):
   - A “socketless” read implementation that sources bytes from host-provided memory/buffers
   - A write implementation bridged to host_ops->reserve_response/commit_response/flush
4. Library entry: create pglite_init/exec_protocol/close; set up Port and install vtables per-instance (guard with thread-local if needed)
5. Remove compatibility shims: interactive_one.c, CMA read-path conditionals; WASM/mobile adapters implement host ops only

Risk containment:

- All edits localized to pqcomm + new libpglite glue; rest of backend remains untouched
- Default builds (sockets) unaffected; host-less builds use the default vtables

---

## Open design choices and risks

Choices

- Read vtable shape: span-based vs buffer-fill
- Zero-copy write interface: add reserve/commit now or later

Risks and mitigations

1. Error handling with longjmp

- Ensure callbacks are invoked only in safe regions; wrap with PG_TRY/PG_CATCH; convert to error codes.

2. WASM boundary overhead

- Keep crossings coarse; avoid per-frame/row hooks; parse on host.

3. Initialization (initdb/runtime assets)

- Hosts differ in FS access; pass paths/mounts; pre-bundle assets for WASM.

4. Buffer semantics and alignment

- Provide flat buffers with explicit lengths; avoid offset conventions.

5. ABI stability

- C headers only; fixed-width types; explicit alignment; versioned abi_version; no exceptions.

---

## Iteration plan

1. Add PQcomm read-vtable and bridge write path to host ops; compile-time gated; default to sockets when not installed.
2. Implement libpglite entrypoints (init/exec/close) with transport-only host ops.
3. Remove interactive_one.c and platform-specific CMA paths from the build; delete mobile-build wrappers that are no longer needed.
4. Wire up logging; notices/notifications remain on the transport (no extra hooks).
5. Add capability flags and assertions; add cross-platform unit/integration tests.
