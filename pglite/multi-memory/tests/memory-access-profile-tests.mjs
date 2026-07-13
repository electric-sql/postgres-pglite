#!/usr/bin/env node

import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

const [wasmPath, reportPath] = process.argv.slice(2)
const report = JSON.parse(await readFile(reportPath, 'utf8'))
const kinds = report.abi.memoryAccessProfileKinds
assert.equal(report.abi.memoryAccessProfiling, true)
const counts = [0, 0]
const privateMemory = new WebAssembly.Memory({
  initial: 2,
  maximum: 16,
  shared: true,
})
const globalMemory = new WebAssembly.Memory({
  initial: 2,
  maximum: 16,
  shared: true,
})
const stack = new WebAssembly.Global({ value: 'i32', mutable: true }, 112)
const slot = new WebAssembly.Global({ value: 'i32', mutable: true }, 144)
const { instance } = await WebAssembly.instantiate(await readFile(wasmPath), {
  env: { memory: privateMemory, __stack_pointer: stack },
  'GOT.mem': { private_slot: slot },
  pglite: {
    global_memory: globalMemory,
    scoped_memory: privateMemory,
    profile_memory_access(classification, kind) {
      assert.ok(classification === 0 || classification === 1)
      assert.ok(kind >= 0 && kind < kinds.length)
      counts[classification]++
    },
  },
})
new DataView(privateMemory.buffer).setInt32(176, 6, true)
new DataView(globalMemory.buffer).setInt32(160, 5, true)

assert.equal(instance.exports.unknown(176), 6)
assert.equal(instance.exports.unknown(0x800000a0), 5)
assert.equal(instance.exports.marked(176), 6)
assert.ok(counts[0] >= 2, `expected direct hits, got ${counts}`)
assert.ok(counts[1] >= 1, `expected generic hits, got ${counts}`)

console.log(
  `memory access profiling: ok; direct=${counts[0]} generic=${counts[1]}`,
)
