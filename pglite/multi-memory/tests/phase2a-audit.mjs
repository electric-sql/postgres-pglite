import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { readFile, writeFile } from 'node:fs/promises'

const [classicPath, oraclePath, repeatPath, reportPath, outputPath] =
  process.argv.slice(2)
if (!outputPath) {
  throw new Error(
    'usage: phase2a-audit.mjs CLASSIC ORACLE REPEAT REPORT OUTPUT',
  )
}

const [classic, oracle, repeat, reportText] = await Promise.all([
  readFile(classicPath),
  readFile(oraclePath),
  readFile(repeatPath),
  readFile(reportPath, 'utf8'),
])
assert.deepEqual(oracle, repeat, 'oracle transformation is not deterministic')

const module = await WebAssembly.compile(oracle)
assert.deepEqual(
  WebAssembly.Module.imports(module).filter(({ kind }) => kind === 'memory'),
  [
    { module: 'env', name: 'memory', kind: 'memory' },
    { module: 'pglite', name: 'global_memory', kind: 'memory' },
    { module: 'pglite', name: 'scoped_memory', kind: 'memory' },
  ],
)
const sections = WebAssembly.Module.customSections(
  module,
  'pglite.multi-memory.abi',
)
assert.equal(sections.length, 1)
const abi = JSON.parse(new TextDecoder().decode(sections[0]))
const report = JSON.parse(reportText)
assert.deepEqual(report.abi, abi)
assert.equal(abi.profile, 'private-only-oracle')
assert.equal(abi.helperCount, 0)
assert.equal(abi.inputSHA256, sha256(classic))
assert.deepEqual(report.rewritten, {})
assert.deepEqual(report.helpers, [])

const directPrivateTotal = Object.values(report.directPrivate).reduce(
  (sum, value) => sum + value,
  0,
)
assert.ok(directPrivateTotal > 300_000)
assert.ok(report.directPrivate.load > 0)
assert.ok(report.directPrivate.store > 0)
assert.equal(report.directPrivate['memory-copy'], 1)
assert.equal(report.directPrivate['memory-fill'], 1)

const result = {
  schema: 1,
  status: 'pass',
  classicBytes: classic.byteLength,
  oracleBytes: oracle.byteLength,
  sizeRatio: oracle.byteLength / classic.byteLength,
  classicSHA256: sha256(classic),
  oracleSHA256: sha256(oracle),
  directPrivateTotal,
  directPrivate: report.directPrivate,
  allowlisted: report.allowlisted,
  abi,
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log(
  `Phase 2A oracle audit: ${directPrivateTotal} direct-private sites, ${result.sizeRatio.toFixed(4)}x bytes`,
)

function sha256(bytes) {
  return createHash('sha256').update(bytes).digest('hex')
}
