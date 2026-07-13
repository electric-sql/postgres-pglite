#!/usr/bin/env node

import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

const [wasmPath, reportPath] = process.argv.slice(2)
const memory = new WebAssembly.Memory({
  initial: 2,
  maximum: 16,
  shared: true,
})
const stack = new WebAssembly.Global({ value: 'i32', mutable: true }, 112)
const slot = new WebAssembly.Global({ value: 'i32', mutable: true }, 144)
const { instance } = await WebAssembly.instantiate(await readFile(wasmPath), {
  env: { memory, __stack_pointer: stack },
  'GOT.mem': { private_slot: slot },
})
new DataView(memory.buffer).setInt32(176, 42, true)
assert.equal(instance.exports.marked(176), 42)
assert.equal(instance.exports.marked_parameter(176), 42)
assert.equal(instance.exports.conditional_marked(176, 1), 42)

const report = JSON.parse(await readFile(reportPath, 'utf8'))
assert.equal(report.mode, 'strip-private-identities-only')
assert.equal(report.privateIdentityExports[0], 'pgl_private_pointer')
assert.equal(report.removedPrivateIdentityCalls, 3)

console.log('private identity stripping: ok')
