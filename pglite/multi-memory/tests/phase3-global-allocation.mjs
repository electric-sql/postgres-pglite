#!/usr/bin/env node

import assert from 'node:assert/strict'
import { readFile, writeFile } from 'node:fs/promises'

const [modulePath, outputPath] = process.argv.slice(2)
if (!outputPath) {
  throw new Error(
    'usage: phase3-global-allocation.mjs TRANSFORMED_WASM OUTPUT',
  )
}

const privateMemory = makeMemory(32768)
const globalMemory = makeMemory(16384)
const module = await WebAssembly.compile(await readFile(modulePath))
const instance = await WebAssembly.instantiate(module, {
  env: { memory: privateMemory },
  pglite: {
    global_memory: globalMemory,
    scoped_memory: privateMemory,
  },
})
const api = instance.exports
const privateView = new DataView(privateMemory.buffer)
const globalView = new DataView(globalMemory.buffer)

let bump = 4096
const allocateGlobal = (bytes, alignment = 8) => {
  bump = Math.ceil(bump / alignment) * alignment
  const offset = bump
  bump += bytes
  assert.ok(bump < 0x40000000)
  return (0x80000000 | offset) | 0
}
const offsetOf = (pointer) => pointer & 0x3fffffff

const first = allocateGlobal(16)
api.store_u32(first, 0x12345678)
assert.equal(globalView.getUint32(offsetOf(first), true), 0x12345678)
assert.equal(privateView.getUint32(offsetOf(first), true), 0)
assert.equal(api.load_u32(first) >>> 0, 0x12345678)
assert.equal(api.atomic_add_u32(first, 7) >>> 0, 0x12345678)
assert.equal(api.load_u32(first) >>> 0, 0x1234567f)

const second = allocateGlobal(16)
api.copy_bytes(second, first, 4)
assert.equal(api.load_u32(second) >>> 0, 0x1234567f)

api.store_u32(8192, 0x5a17c0de)
assert.equal(privateView.getUint32(8192, true), 0x5a17c0de)
assert.equal(globalView.getUint32(8192, true), 0)

const result = {
  schema: 1,
  status: 'pass',
  sharedPrivate: privateMemory.buffer instanceof SharedArrayBuffer,
  sharedGlobal: globalMemory.buffer instanceof SharedArrayBuffer,
  distinctDomains: privateMemory !== globalMemory,
  scopedAliasesPrivate: true,
  privateMaximumPages: 32768,
  globalMaximumPages: 16384,
  allocatedGlobalBytes: bump - 4096,
  atomicResult: api.load_u32(first) >>> 0,
  copiedResult: api.load_u32(second) >>> 0,
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log('Phase 3 synthetic tagged global allocation: PASS')

function makeMemory(maximum) {
  return new WebAssembly.Memory({
    initial: 1,
    maximum,
    shared: true,
  })
}
