#!/usr/bin/env node

import assert from 'node:assert/strict'
import { readFile, readdir, rm, writeFile } from 'node:fs/promises'
import { dirname, join, relative } from 'node:path'
import { pathToFileURL } from 'node:url'

const [
  seedEntry,
  sharedEntry,
  socketEntry,
  seedWasmPath,
  candidatePath,
  dataDir,
  overlayRoot,
  regressionInputRoot,
  readyPath,
  portText,
] = process.argv.slice(2)
if (!portText) {
  throw new Error(
    'usage: phase3-regress-server.mjs SEED_ENTRY SHARED_ENTRY SOCKET_ENTRY SEED_WASM CANDIDATE PGDATA OVERLAY INPUT READY PORT',
  )
}

const port = Number(portText)
assert.ok(Number.isInteger(port) && port > 0 && port < 65536)
assert.ok(['arm64', 'x64'].includes(process.arch))

const [{ PGlite: SeedPGlite }, { PGlite: SharedPGlite }, socketModule] =
  await Promise.all([
    import(pathToFileURL(seedEntry)),
    import(pathToFileURL(sharedEntry)),
    import(pathToFileURL(socketEntry)),
  ])
const { PGLiteSocketServer } = socketModule
const [seedModule, candidateModule] = await Promise.all([
  compile(seedWasmPath),
  compile(candidatePath),
])

const seed = await SeedPGlite.create({ pgliteWasmModule: seedModule })
let seedBytes
try {
  await seed.exec('CREATE DATABASE regression TEMPLATE template0')
  seedBytes = new Uint8Array(
    await (await seed.dumpDataDir('none')).arrayBuffer(),
  )
} finally {
  await seed.close()
}

await rm(dataDir, { recursive: true, force: true })
await rm(readyPath, { force: true })

const privateMemory = new WebAssembly.Memory({
  initial: 2048,
  maximum: 32768,
  shared: true,
})
const globalMemory = new WebAssembly.Memory({
  initial: 2048,
  maximum: 16384,
  shared: true,
})
const state = { instantiated: false }
const loader = {
  name: 'phase3-regress-shared-loader',
  async setup(_pg, options) {
    return {
      emscriptenOpts: {
        ...options,
        wasmMemory: privateMemory,
        instantiateWasm(imports, successCallback) {
          assert.strictEqual(imports.env.memory, privateMemory)
          imports.pglite = {
            ...(imports.pglite ?? {}),
            global_memory: globalMemory,
            scoped_memory: privateMemory,
          }
          const instance = new WebAssembly.Instance(candidateModule, imports)
          state.instantiated = true
          successCallback(instance, candidateModule)
          return {}
        },
      },
    }
  },
}

const db = await SharedPGlite.create({
  dataDir,
  database: 'regression',
  loadDataDir: new Blob([seedBytes]),
  pgliteWasmModule: candidateModule,
  startParams: [
    ...SharedPGlite.defaultStartParams,
    '-c',
    'datestyle=Postgres, MDY',
    '-c',
    'intervalstyle=postgres_verbose',
    '-c',
    'timezone=America/Los_Angeles',
  ],
  extensions: { phase3RegressLoader: loader },
})
assert.equal(state.instantiated, true)
assert.ok(privateMemory.buffer instanceof SharedArrayBuffer)
assert.ok(globalMemory.buffer instanceof SharedArrayBuffer)
assert.notStrictEqual(privateMemory, globalMemory)

await copyTreeToWasm(db.Module.FS, overlayRoot, '/pglite')
await copyTreeToWasm(
  db.Module.FS,
  join(regressionInputRoot, 'data'),
  join(regressionInputRoot, 'data'),
)
assert.equal(
  db.Module.FS.analyzePath('/pglite/lib/postgresql/regress.so').exists,
  true,
)

const server = new PGLiteSocketServer({
  db,
  host: '127.0.0.1',
  port,
  maxConnections: 1,
})
await server.start()
await writeFile(
  readyPath,
  `${JSON.stringify(
    {
      schema: 1,
      status: 'ready',
      pid: process.pid,
      host: '127.0.0.1',
      port,
      architecture: process.arch,
      sharedPrivate: privateMemory.buffer instanceof SharedArrayBuffer,
      sharedGlobal: globalMemory.buffer instanceof SharedArrayBuffer,
      distinctDomains: privateMemory !== globalMemory,
      transformed: true,
    },
    null,
    2,
  )}\n`,
)

let stopping = false
const stop = async (signal) => {
  if (stopping) return
  stopping = true
  try {
    await server.stop()
    await db.close()
    await rm(readyPath, { force: true })
    process.exit(signal ? 0 : 1)
  } catch (error) {
    console.error(error)
    process.exit(1)
  }
}
process.on('SIGTERM', () => void stop('SIGTERM'))
process.on('SIGINT', () => void stop('SIGINT'))
process.on('uncaughtException', (error) => {
  console.error(error)
  void stop()
})
process.on('unhandledRejection', (error) => {
  console.error(error)
  void stop()
})

async function compile(path) {
  return WebAssembly.compile(await readFile(path))
}

async function copyTreeToWasm(fs, hostRoot, wasmRoot) {
  fs.mkdirTree(wasmRoot)
  for (const entry of await readdir(hostRoot, { withFileTypes: true })) {
    const hostPath = join(hostRoot, entry.name)
    const wasmPath = join(wasmRoot, relative(hostRoot, hostPath))
    if (entry.isDirectory()) {
      await copyTreeToWasm(fs, hostPath, wasmPath)
    } else if (entry.isFile()) {
      fs.mkdirTree(dirname(wasmPath))
      fs.writeFile(wasmPath, await readFile(hostPath))
    }
  }
}
