import assert from 'node:assert/strict'
import { readFile, writeFile } from 'node:fs/promises'

const [reportPath, performancePath, outputPath] = process.argv.slice(2)
const [report, performance] = await Promise.all(
  [reportPath, performancePath].map((path) =>
    readFile(path, 'utf8').then(JSON.parse),
  ),
)
assert.equal(report.abi.profile, 'three-domain-provenance')
const clones = report.functions
  .filter((entry) => entry.privateCloneOf !== null)
  .map((entry) => ({
    source: entry.privateCloneOf,
    directStaticOperations: sum(entry.directPrivateOperations),
    genericStaticOperations: sum(entry.genericOperations),
  }))
assert.ok(clones.length > 0)
const result = {
  schema: 1,
  status: performance.status,
  clones,
  artifactSizeRatio: performance.transformed.bytes / performance.classic.bytes,
  worstThroughputRatio: performance.worstThroughputRatio,
  workloadRatios: performance.ratios,
  next:
    performance.status === 'pass'
      ? 'run the complete Phase 2 exit gate'
      : 'add pointer-field/root metadata for the ranked residual generic sites',
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log(
  `Phase 2D guarded clones: ${clones.length} clones, ${result.worstThroughputRatio.toFixed(2)}x worst case: ${result.status}`,
)

function sum(values) {
  return Object.values(values).reduce((total, value) => total + value, 0)
}
