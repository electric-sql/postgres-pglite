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
const privateView = new DataView(privateMemory.buffer)
const globalView = new DataView(globalMemory.buffer)

for (const [address, value] of [
  [96, 1],
  [104, 2],
  [128, 3],
  [144, 4],
  [176, 6],
  [180, 7],
  [184, 9],
]) {
  privateView.setInt32(address, value, true)
}
globalView.setInt32(160, 5, true)
globalView.setInt32(164, 8, true)

assert.equal(instance.exports.constant(), 1)
assert.equal(instance.exports.stack(), 2)
assert.equal(instance.exports.allocator_and_internal(), 3)
assert.equal(instance.exports.got(), 4)
assert.equal(instance.exports.unknown(0x800000a0), 5)
assert.equal(instance.exports.unknown(176), 6)
assert.equal(instance.exports.marked(176), 6)
assert.equal(instance.exports.marked_parameter(184), 9)
assert.equal(instance.exports.conditional_marked(184, 1), 9)
assert.equal(instance.exports.conditional_marked(0x800000a4, 0), 8)
assert.equal(instance.exports.loop(176, 2), 13)
assert.equal(instance.exports.loop(0x800000a0, 2), 13)
assert.equal(report.abi.profile, 'two-domain-provenance')
assert.equal(report.privateReturnExports[0], 'palloc')
assert.equal(report.privateIdentityExports[0], 'pgl_private_pointer')
assert.equal(report.removedPrivateIdentityCalls, 3)
assert.equal(report.explicitPrivateParameters.length, 1)
assert.equal(report.privateCloneExports[0], 'unknown:0')
assert.equal(report.privateCloneExports[1], 'loop:0')
assert.ok(report.inferredPrivateParameters >= 1)
assert.ok(report.directPrivate.load >= 4)
assert.ok(report.rewritten.load >= 1)
assert.equal(
  report.directPrivateProofs['constant-local-flow'],
  report.directPrivate.load,
)

console.log('provenance semantics: ok')
