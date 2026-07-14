#!/usr/bin/env node

import { readFileSync } from 'node:fs'
import { parentPort, workerData } from 'node:worker_threads'

const {
  gluePath,
  dataPath,
  module,
  privateMemory,
  globalMemory,
  dataDirectory,
  pid,
} = workerData
const { default: createPostgres } = await import(gluePath)
const data = readFileSync(dataPath)

const postgres = await createPostgres({
  noInitialRun: true,
  noExitRuntime: true,
  wasmMemory: privateMemory,
  getPreloadedPackage: () =>
    data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength),
  instantiateWasm(imports, success) {
    imports.pglite = {
      ...(imports.pglite ?? {}),
      global_memory: globalMemory,
      scoped_memory: privateMemory,
    }
    WebAssembly.instantiate(module, imports).then((instance) =>
      success(instance, module),
    )
    return {}
  },
  preRun: [
    (mod) => {
      mod.FS.mkdir('/phase4-nodefs')
      mod.FS.mount(
        mod.FS.filesystems.NODEFS,
        { root: dataDirectory },
        '/phase4-nodefs',
      )
    },
  ],
})

const spawn = postgres.addFunction(() => 4242, 'ippii')
const getpid = postgres.addFunction(() => pid, 'i')
const kill = postgres.addFunction(() => 0, 'iii')
const waitpid = postgres.addFunction(() => 0, 'iipi')
postgres._pgl_set_process_host(spawn, getpid, kill, waitpid)

const signalPoll = postgres.addFunction(() => 0, 'i')
const signalMask = postgres.addFunction(() => {}, 'vi')
const timer = postgres.addFunction(() => 0, 'idd')
postgres._pgl_set_signal_host(signalPoll, signalMask, timer)

const futexWait = postgres.addFunction(() => 0, 'ipid')
const futexWake = postgres.addFunction((pointer, count) => {
  const offset = (pointer >>> 0) & 0x3fffffff
  return Atomics.notify(new Int32Array(globalMemory.buffer), offset / 4, count)
}, 'ipi')
postgres._pgl_set_futex_host(futexWait, futexWake)

Atomics.add(new Int32Array(globalMemory.buffer), 0, 1)
new Uint8Array(privateMemory.buffer)[0] = pid & 0xff

parentPort.postMessage({
  type: 'ready',
  pid: postgres._pgl_getpid(),
  spawnResult: postgres._pgl_spawn_backend(1, 1, -1, 0),
  privateByte: new Uint8Array(privateMemory.buffer)[0],
  sharedMemory: privateMemory.buffer instanceof SharedArrayBuffer,
  scopedAliasesPrivate: true,
  tableIndex: spawn,
})

parentPort.on('message', (message) => {
  if (message.type === 'write') {
    postgres.FS.writeFile('/phase4-nodefs/parameter-file', message.value)
    parentPort.postMessage({ type: 'wrote' })
  } else if (message.type === 'read') {
    parentPort.postMessage({
      type: 'read',
      value: postgres.FS.readFile('/phase4-nodefs/parameter-file', {
        encoding: 'utf8',
      }),
    })
  } else if (message.type === 'close') {
    postgres.FS.quit()
    process.exit(0)
  }
})
