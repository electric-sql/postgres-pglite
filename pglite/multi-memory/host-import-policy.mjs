/*
 * Audited against Emscripten 3.1.74 generated glue. A `p` in an Emscripten
 * signature means pointer-width, not necessarily a C data pointer, so every
 * such parameter is explicitly classified here. Lengths are expressions in
 * terms of the zero-based Wasm parameters and, where noted, pointed-to data.
 */

const pointer = (index, length, direction = 'in', nullable = false) => ({
  index,
  length,
  direction,
  nullable,
})

const tagged = (
  pointers,
  pointerSizedScalars = [],
  returnPointer = 'none',
) => ({ class: 'tagged', pointers, pointerSizedScalars, returnPointer })

const privateOnly = (
  pointers,
  pointerSizedScalars = [],
  returnPointer = 'none',
) => ({ class: 'private-only', pointers, pointerSizedScalars, returnPointer })

export const pointerImportPolicy = {
  'env.getaddrinfo': tagged([
    pointer(0, 'cstring', 'in', true),
    pointer(1, 'cstring', 'in', true),
    pointer(2, '32-byte struct addrinfo', 'in', true),
    pointer(3, '4-byte result pointer', 'out'),
  ]),
  'env.__assert_fail': tagged([
    pointer(0, 'cstring'),
    pointer(1, 'cstring', 'in', true),
    pointer(3, 'cstring', 'in', true),
  ]),
  'env.getnameinfo': tagged([
    pointer(0, 'parameter 1 bytes'),
    pointer(2, 'parameter 3 bytes', 'out', true),
    pointer(4, 'parameter 5 bytes', 'out', true),
  ]),
  'wasi_snapshot_preview1.environ_sizes_get': privateOnly([
    pointer(0, '4-byte environment count', 'out'),
    pointer(1, '4-byte environment buffer size', 'out'),
  ]),
  'wasi_snapshot_preview1.environ_get': privateOnly([
    pointer(0, '4 * environment count pointer array', 'out'),
    pointer(1, 'environment byte size', 'out'),
  ]),
  'env._tzset_js': privateOnly([
    pointer(0, '4-byte timezone offset', 'out'),
    pointer(1, '4-byte daylight flag', 'out'),
    pointer(2, '17-byte timezone name', 'out'),
    pointer(3, '17-byte timezone name', 'out'),
  ]),
  'env.__syscall_faccessat': tagged([pointer(1, 'cstring')]),
  'env.__syscall_chdir': tagged([pointer(0, 'cstring')]),
  'env.__syscall_chmod': tagged([pointer(0, 'cstring')]),
  'env.__syscall_fchownat': tagged([pointer(1, 'cstring')]),
  'wasi_snapshot_preview1.clock_time_get': privateOnly([
    pointer(2, '8-byte timestamp', 'out'),
  ]),
  'env._dlopen_js': privateOnly(
    [pointer(0, 'private dynamic-loader handle')],
    [],
    'private',
  ),
  'env._dlsym_js': privateOnly(
    [pointer(1, 'cstring'), pointer(2, '4-byte dynamic symbol index', 'out')],
    [0],
    'private',
  ),
  'env.__syscall_openat': tagged(
    [
      pointer(1, 'cstring'),
      pointer(3, '4-byte variadic mode argument', 'in', true),
    ],
    [],
  ),
  'env.__syscall_fcntl64': tagged([
    pointer(
      2,
      'opcode-dependent variadic argument; nested pointers decoded by opcode',
    ),
  ]),
  'env.__syscall_ioctl': tagged([
    pointer(
      2,
      'opcode-dependent variadic argument; nested pointers decoded by opcode',
    ),
  ]),
  'wasi_snapshot_preview1.fd_write': tagged(
    [
      pointer(
        1,
        'parameter 2 entries of 8-byte iovec; each base is tagged',
        'in',
      ),
      pointer(3, '4-byte byte count', 'out'),
    ],
    [2],
  ),
  'wasi_snapshot_preview1.fd_read': tagged(
    [
      pointer(
        1,
        'parameter 2 entries of 8-byte iovec; each base is tagged',
        'inout',
      ),
      pointer(3, '4-byte byte count', 'out'),
    ],
    [2],
  ),
  'env.__syscall_fstat64': tagged([
    pointer(1, '112-byte musl wasm32 struct stat', 'out'),
  ]),
  'env.__syscall_stat64': tagged([
    pointer(0, 'cstring'),
    pointer(1, '112-byte musl wasm32 struct stat', 'out'),
  ]),
  'env.__syscall_newfstatat': tagged([
    pointer(1, 'cstring'),
    pointer(2, '112-byte musl wasm32 struct stat', 'out'),
  ]),
  'env.__syscall_lstat64': tagged([
    pointer(0, 'cstring'),
    pointer(1, '112-byte musl wasm32 struct stat', 'out'),
  ]),
  'env.__syscall_getcwd': tagged([pointer(0, 'parameter 1 bytes', 'out')], [1]),
  'wasi_snapshot_preview1.fd_fdstat_get': privateOnly([
    pointer(1, '24-byte WASI fdstat', 'out'),
  ]),
  'wasi_snapshot_preview1.fd_seek': privateOnly([
    pointer(3, '8-byte file offset', 'out'),
  ]),
  'env.__syscall_mkdirat': tagged([pointer(1, 'cstring')]),
  'env._localtime_js': privateOnly([
    pointer(1, '44-byte musl wasm32 struct tm', 'out'),
  ]),
  'env._gmtime_js': privateOnly([
    pointer(1, '44-byte musl wasm32 struct tm', 'out'),
  ]),
  'env._munmap_js': tagged([pointer(0, 'parameter 1 bytes', 'in')], [1]),
  'env._mmap_js': privateOnly(
    [
      pointer(5, '4-byte allocation flag', 'out'),
      pointer(6, '4-byte mapped pointer', 'out'),
    ],
    [0],
  ),
  'env.__syscall_pipe': privateOnly([
    pointer(0, 'two 4-byte file descriptors', 'out'),
  ]),
  'wasi_snapshot_preview1.fd_pread': tagged(
    [
      pointer(
        1,
        'parameter 2 entries of 8-byte iovec; each base is tagged',
        'inout',
      ),
      pointer(4, '4-byte byte count', 'out'),
    ],
    [2],
  ),
  'wasi_snapshot_preview1.fd_pwrite': tagged(
    [
      pointer(
        1,
        'parameter 2 entries of 8-byte iovec; each base is tagged',
        'in',
      ),
      pointer(4, '4-byte byte count', 'out'),
    ],
    [2],
  ),
  'env.__call_sighandler': {
    class: 'opaque-indirect',
    pointers: [],
    pointerSizedScalars: [0],
    returnPointer: 'none',
  },
  'env.__syscall_getdents64': tagged(
    [pointer(1, 'parameter 2 bytes of dirent records', 'out')],
    [2],
  ),
  'env.__syscall_readlinkat': tagged(
    [pointer(1, 'cstring'), pointer(2, 'parameter 3 bytes', 'out')],
    [3],
  ),
  'env.__syscall_renameat': tagged([
    pointer(1, 'cstring'),
    pointer(3, 'cstring'),
  ]),
  'env.__syscall_rmdir': tagged([pointer(0, 'cstring')]),
  'env.__syscall__newselect': tagged([
    pointer(1, 'ceil(parameter 0 / 8) bytes', 'inout', true),
    pointer(2, 'ceil(parameter 0 / 8) bytes', 'inout', true),
    pointer(3, 'ceil(parameter 0 / 8) bytes', 'inout', true),
    pointer(4, '8-byte timeval', 'inout', true),
  ]),
  'env.__syscall_symlinkat': tagged([
    pointer(0, 'cstring'),
    pointer(2, 'cstring'),
  ]),
  'env.emscripten_get_heap_max': {
    class: 'scalar',
    pointers: [],
    pointerSizedScalars: [],
    returnPointer: 'none',
    pointerSizedReturn: true,
  },
  'env.__syscall_truncate64': tagged([pointer(0, 'cstring')]),
  'env.__syscall_unlinkat': tagged([pointer(1, 'cstring')]),
  'env.__syscall_utimensat': tagged([
    pointer(1, 'cstring', 'in', true),
    pointer(2, 'two 16-byte wasm32 timespec values', 'in', true),
  ]),
  'env.emscripten_resize_heap': {
    class: 'scalar',
    pointers: [],
    pointerSizedScalars: [0],
    returnPointer: 'none',
  },
  'env.__syscall_accept4': tagged([
    pointer(1, 'sockaddr size read from parameter 2', 'out', true),
    pointer(2, '4-byte socklen', 'inout', true),
  ]),
  'env.__syscall_bind': tagged(
    [pointer(1, 'parameter 2 bytes of sockaddr')],
    [2],
  ),
  'env.__syscall_connect': tagged(
    [pointer(1, 'parameter 2 bytes of sockaddr')],
    [2],
  ),
  'env.__syscall_recvfrom': tagged(
    [
      pointer(1, 'parameter 2 bytes', 'out'),
      pointer(4, 'sockaddr size read from parameter 5', 'out', true),
      pointer(5, '4-byte socklen', 'inout', true),
    ],
    [2],
  ),
  'env.__syscall_sendto': tagged(
    [
      pointer(1, 'parameter 2 bytes'),
      pointer(4, 'parameter 5 bytes of sockaddr', 'in', true),
    ],
    [2, 5],
  ),
  'wasi_snapshot_preview1.random_get': tagged(
    [pointer(0, 'parameter 1 bytes', 'out')],
    [1],
  ),
  'env.__syscall_fchmodat2': tagged([pointer(1, 'cstring')]),
  'env.__syscall_statfs64': tagged(
    [
      pointer(0, 'cstring'),
      pointer(2, '64-byte musl wasm32 struct statfs', 'out'),
    ],
    [1],
  ),
}
