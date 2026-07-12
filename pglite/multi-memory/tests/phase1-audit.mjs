import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { readFile, writeFile } from 'node:fs/promises'

const [classicPath, transformedPath, repeatPath, reportPath, outputPath] =
  process.argv.slice(2)

if (!outputPath) {
  throw new Error(
    'usage: phase1-audit.mjs CLASSIC TRANSFORMED REPEAT REPORT OUTPUT',
  )
}

const [classic, transformed, repeat, reportText] = await Promise.all([
  readFile(classicPath),
  readFile(transformedPath),
  readFile(repeatPath),
  readFile(reportPath, 'utf8'),
])
assert.deepEqual(transformed, repeat, 'transformation is not deterministic')

const module = await WebAssembly.compile(transformed)
const memoryImports = WebAssembly.Module.imports(module).filter(
  ({ kind }) => kind === 'memory',
)
assert.deepEqual(memoryImports, [
  { module: 'env', name: 'memory', kind: 'memory' },
  { module: 'pglite', name: 'global_memory', kind: 'memory' },
  { module: 'pglite', name: 'scoped_memory', kind: 'memory' },
])

const sections = WebAssembly.Module.customSections(
  module,
  'pglite.multi-memory.abi',
)
assert.equal(sections.length, 1, 'expected exactly one ABI custom section')
const abi = JSON.parse(new TextDecoder().decode(sections[0]))
const report = JSON.parse(reportText)
assert.deepEqual(report.abi, abi)
assert.equal(abi.pointerABI, 'pglite-tagged-i32-v1')
assert.equal(abi.profile, 'two-domain-generic-private-fast-path')
assert.equal(abi.privateTag, 0)
assert.equal(abi.globalTag, 2)
assert.equal(abi.reservedTag, 3)
assert.equal(abi.inputSHA256, sha256(classic))

const rewrittenTotal = Object.values(report.rewritten).reduce(
  (sum, value) => sum + value,
  0,
)
assert.ok(
  rewrittenTotal > 300_000,
  'real artifact rewrite inventory is too small',
)
assert.ok(report.rewritten.load > 0)
assert.ok(report.rewritten.store > 0)
assert.ok(report.rewritten['memory-copy'] > 0)
assert.ok(report.rewritten['memory-fill'] > 0)
assert.equal(report.allowlisted['memory-size-private'], 1)
assert.equal(report.helpers.length, abi.helperCount)

const result = {
  schema: 1,
  classicBytes: classic.byteLength,
  transformedBytes: transformed.byteLength,
  sizeRatio: transformed.byteLength / classic.byteLength,
  classicSHA256: sha256(classic),
  transformedSHA256: sha256(transformed),
  rewrittenTotal,
  rewritten: report.rewritten,
  allowlisted: report.allowlisted,
  helperCount: report.helpers.length,
  memoryImports,
  abi,
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log(
  `Phase 1 audit: ${rewrittenTotal} rewritten sites, ${report.helpers.length} helpers, ${result.sizeRatio.toFixed(2)}x bytes`,
)

function sha256(bytes) {
  return createHash('sha256').update(bytes).digest('hex')
}
