#!/usr/bin/env node

import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { readFile, writeFile } from 'node:fs/promises'

class Reader {
  constructor(bytes) {
    this.bytes = bytes
    this.offset = 0
  }

  byte() {
    assert.ok(this.offset < this.bytes.length, 'unexpected end of Wasm')
    return this.bytes[this.offset++]
  }

  take(length) {
    const result = this.bytes.subarray(this.offset, this.offset + length)
    assert.equal(result.length, length, 'unexpected end of Wasm')
    this.offset += length
    return result
  }

  uleb() {
    let value = 0
    let shift = 0
    while (true) {
      const byte = this.byte()
      value += (byte & 0x7f) * 2 ** shift
      if ((byte & 0x80) === 0) return value
      shift += 7
      assert.ok(shift <= 49, 'oversized Wasm unsigned integer')
    }
  }

  name() {
    return new TextDecoder().decode(this.take(this.uleb()))
  }
}

const [sourcePath, classicPath, candidatePath, reportPath, outputPath] =
  process.argv.slice(2)
if (!outputPath) {
  throw new Error(
    'usage: phase3-audit.mjs SOURCE CLASSIC CANDIDATE REPORT OUTPUT',
  )
}

const sourceBytes = await readFile(sourcePath)
const classicBytes = await readFile(classicPath)
const candidateBytes = await readFile(candidatePath)
const report = JSON.parse(await readFile(reportPath, 'utf8'))

const [sourceModule, classicModule, candidateModule] = await Promise.all([
  WebAssembly.compile(sourceBytes),
  WebAssembly.compile(classicBytes),
  WebAssembly.compile(candidateBytes),
])

const sourceMemories = memoryImports(sourceBytes)
const classicMemories = memoryImports(classicBytes)
const candidateMemories = memoryImports(candidateBytes)
const sourcePrivate = requiredMemory(sourceMemories, 'env', 'memory')
const classicPrivate = requiredMemory(classicMemories, 'env', 'memory')
const candidatePrivate = requiredMemory(candidateMemories, 'env', 'memory')
const candidateGlobal = requiredMemory(
  candidateMemories,
  'pglite',
  'global_memory',
)
const candidateScoped = requiredMemory(
  candidateMemories,
  'pglite',
  'scoped_memory',
)

assert.deepEqual(classicPrivate, sourcePrivate)
assert.deepEqual(candidatePrivate, sourcePrivate)
assert.equal(sourcePrivate.shared, true, 'source memory must be shared')
assert.equal(sourcePrivate.memory64, false, 'memory64 is not the v1 ABI')
assert.notEqual(sourcePrivate.maximum, undefined, 'shared memory needs a maximum')
assert.deepEqual(
  descriptor(candidateScoped),
  descriptor(candidatePrivate),
  'the reserved scoped import must be compatible with the v1 memory-0 alias',
)
assert.equal(candidateGlobal.shared, true, 'global memory must be shared')
assert.equal(candidateGlobal.memory64, false)
assert.notEqual(candidateGlobal.maximum, undefined)

assert.deepEqual(
  WebAssembly.Module.imports(classicModule),
  WebAssembly.Module.imports(sourceModule),
  'stripping provenance must not change the classic shared import ABI',
)
const candidateImportNames = WebAssembly.Module.imports(candidateModule).map(
  ({ module, name, kind }) => `${module}.${name}:${kind}`,
)
assert.ok(candidateImportNames.includes('pglite.global_memory:memory'))
assert.ok(candidateImportNames.includes('pglite.scoped_memory:memory'))

const targetFeatures = readTargetFeatures(candidateModule)
for (const feature of ['atomics', 'bulk-memory', 'multimemory']) {
  assert.equal(
    targetFeatures.get(feature),
    '+',
    `candidate must require ${feature}`,
  )
}

const abiSections = WebAssembly.Module.customSections(
  candidateModule,
  'pglite.multi-memory.abi',
)
assert.equal(abiSections.length, 1, 'candidate must carry exactly one ABI stamp')
const abi = JSON.parse(new TextDecoder().decode(abiSections[0]))
assert.equal(abi.pointerABI, 'pglite-tagged-i32-v1')
assert.match(abi.features, /atomics/)
assert.match(abi.features, /multimemory/)
assert.equal(report.abi.pointerABI, 'pglite-tagged-i32-v1')
assert.match(report.abi.features, /atomics/)
assert.equal(report.abi.globalApertureBytes, 1 << 30)
assert.ok(
  Object.values(report.rewritten).reduce((total, count) => total + count, 0) >
    0,
  'the PostgreSQL artifact must contain rewritten memory operations',
)

const result = {
  schema: 1,
  status: 'pass',
  sourceSha256: sha256(sourceBytes),
  classicSha256: sha256(classicBytes),
  candidateSha256: sha256(candidateBytes),
  sourceBytes: sourceBytes.byteLength,
  classicBytes: classicBytes.byteLength,
  candidateBytes: candidateBytes.byteLength,
  memories: {
    private: candidatePrivate,
    global: candidateGlobal,
    scoped: candidateScoped,
  },
  targetFeatures: Object.fromEntries(targetFeatures),
  rewrittenOperations: Object.values(report.rewritten).reduce(
    (total, count) => total + count,
    0,
  ),
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log('Phase 3 shared artifact audit: PASS')

function descriptor(memory) {
  return {
    initial: memory.initial,
    maximum: memory.maximum,
    shared: memory.shared,
    memory64: memory.memory64,
  }
}

function requiredMemory(memories, module, name) {
  const memory = memories.find(
    (candidate) => candidate.module === module && candidate.name === name,
  )
  assert.ok(memory, `missing memory import ${module}.${name}`)
  return memory
}

function sha256(bytes) {
  return createHash('sha256').update(bytes).digest('hex')
}

function readTargetFeatures(module) {
  const sections = WebAssembly.Module.customSections(module, 'target_features')
  assert.equal(sections.length, 1, 'candidate must carry target_features')
  const reader = new Reader(new Uint8Array(sections[0]))
  const count = reader.uleb()
  const features = new Map()
  for (let index = 0; index < count; index++) {
    const prefix = String.fromCharCode(reader.byte())
    assert.ok(prefix === '+' || prefix === '-', 'invalid target feature prefix')
    features.set(reader.name(), prefix)
  }
  assert.equal(reader.offset, reader.bytes.length)
  return features
}

function memoryImports(bytes) {
  const reader = new Reader(bytes)
  assert.deepEqual([...reader.take(4)], [0, 97, 115, 109])
  assert.deepEqual([...reader.take(4)], [1, 0, 0, 0])
  const memories = []
  while (reader.offset < bytes.length) {
    const id = reader.byte()
    const size = reader.uleb()
    const end = reader.offset + size
    if (id !== 2) {
      reader.offset = end
      continue
    }
    const count = reader.uleb()
    for (let index = 0; index < count; index++) {
      const module = reader.name()
      const name = reader.name()
      const kind = reader.byte()
      if (kind === 0) {
        reader.uleb()
      } else if (kind === 1) {
        reader.byte()
        readLimits(reader)
      } else if (kind === 2) {
        memories.push({ module, name, ...readLimits(reader) })
      } else if (kind === 3) {
        reader.byte()
        reader.byte()
      } else if (kind === 4) {
        reader.uleb()
        reader.uleb()
      } else {
        throw new Error(`unsupported Wasm import kind ${kind}`)
      }
    }
    assert.equal(reader.offset, end, 'malformed import section')
    return memories
  }
  throw new Error('Wasm module has no import section')
}

function readLimits(reader) {
  const flags = reader.uleb()
  const memory64 = (flags & 4) !== 0
  const initial = reader.uleb()
  const maximum = (flags & 1) !== 0 ? reader.uleb() : undefined
  return {
    initial,
    maximum,
    shared: (flags & 2) !== 0,
    memory64,
  }
}
