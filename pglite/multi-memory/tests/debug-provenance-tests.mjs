#!/usr/bin/env node

import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

const [wasmPath, reportPath] = process.argv.slice(2)
const report = JSON.parse(await readFile(reportPath, 'utf8'))
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
  pglite: { global_memory: globalMemory, scoped_memory: privateMemory },
})
new DataView(privateMemory.buffer).setInt32(176, 6, true)
new DataView(globalMemory.buffer).setInt32(160, 5, true)

assert.equal(instance.exports.marked(176), 6)
assert.throws(
  () => instance.exports.marked(0x800000a0),
  WebAssembly.RuntimeError,
)
assert.equal(instance.exports.conditional_marked(0x800000a0, 0), 5)
assert.equal(report.removedPrivateIdentityCalls, 0)

console.log('debug provenance assertions: ok')
