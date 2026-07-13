#!/usr/bin/env node

import assert from 'node:assert/strict'
import { mkdtemp, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { Worker } from 'node:worker_threads'
import { pathToFileURL } from 'node:url'

const [wasmPath, gluePath, dataPath, outputPath] = process.argv.slice(2)
if (!outputPath) {
  throw new Error('usage: phase4-artifact-workers.mjs WASM GLUE DATA OUTPUT')
}

const directory = await mkdtemp(join(tmpdir(), 'pglite-phase4-nodefs-'))
const module = await WebAssembly.compile(
  await import('node:fs').then(({ readFileSync }) => readFileSync(wasmPath)),
)
const globalMemory = sharedMemory()
const privateMemories = [sharedMemory(), sharedMemory()]
const workerPath = new URL('./phase4-artifact-worker.mjs', import.meta.url)
const workers = privateMemories.map(
  (privateMemory, index) =>
    new Worker(workerPath, {
      workerData: {
        gluePath: pathToFileURL(gluePath).href,
        dataPath,
        module,
        privateMemory,
        globalMemory,
        dataDirectory: directory,
        pid: 20_001 + index,
      },
    }),
)

try {
  const ready = await Promise.all(
    workers.map((worker) => message(worker, 'ready')),
  )
  assert.deepEqual(
    ready.map(({ pid }) => pid),
    [20_001, 20_002],
  )
  assert.ok(ready.every(({ spawnResult }) => spawnResult === 4242))
  assert.ok(ready.every(({ sharedMemory }) => sharedMemory))
  assert.ok(ready.every(({ scopedAliasesPrivate }) => scopedAliasesPrivate))
  assert.equal(new Int32Array(globalMemory.buffer)[0], 2)
  assert.equal(new Uint8Array(privateMemories[0].buffer)[0], 20_001 & 0xff)
  assert.equal(new Uint8Array(privateMemories[1].buffer)[0], 20_002 & 0xff)

  workers[0].postMessage({ type: 'write', value: 'backend-parameters-visible' })
  await message(workers[0], 'wrote')
  workers[1].postMessage({ type: 'read' })
  const read = await message(workers[1], 'read')
  assert.equal(read.value, 'backend-parameters-visible')

  await writeFile(
    outputPath,
    `${JSON.stringify(
      {
        schema: 1,
        status: 'pass',
        workers: workers.length,
        pids: ready.map(({ pid }) => pid),
        distinctPrivateMemories: true,
        sharedGlobalCounter: new Int32Array(globalMemory.buffer)[0],
        scopedAlias: 'private',
        nodefsParameterTransport: true,
        perInstanceFunctionTables: ready.map(({ tableIndex }) => tableIndex),
      },
      null,
      2,
    )}\n`,
  )
  console.log('Phase 4 transformed artifact Worker gate: PASS')
} finally {
  for (const worker of workers) worker.postMessage({ type: 'close' })
  await Promise.all(workers.map((worker) => worker.terminate()))
  await rm(directory, { recursive: true, force: true })
}

function sharedMemory() {
  return new WebAssembly.Memory({ initial: 2048, maximum: 32768, shared: true })
}

function message(worker, type) {
  return new Promise((resolve, reject) => {
    const onMessage = (value) => {
      if (value?.type !== type) return
      cleanup()
      resolve(value)
    }
    const onError = (error) => {
      cleanup()
      reject(error)
    }
    const onExit = (code) => {
      cleanup()
      reject(new Error(`artifact Worker exited before ${type}: ${code}`))
    }
    const cleanup = () => {
      worker.off('message', onMessage)
      worker.off('error', onError)
      worker.off('exit', onExit)
    }
    worker.on('message', onMessage)
    worker.on('error', onError)
    worker.on('exit', onExit)
  })
}
