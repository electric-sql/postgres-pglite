import assert from 'node:assert/strict'
import { readFile, writeFile } from 'node:fs/promises'

const [reportPath, performancePath, outputPath] = process.argv.slice(2)
if (!outputPath) {
  throw new Error('usage: phase2b-summary.mjs REPORT PERFORMANCE OUTPUT')
}
const [report, performance] = await Promise.all(
  [reportPath, performancePath].map((path) =>
    readFile(path, 'utf8').then(JSON.parse),
  ),
)
assert.equal(report.abi.profile, 'three-domain-provenance')
const directStaticOperations = sum(report.directPrivate)
const genericStaticOperations = sum(report.rewritten)
assert.equal(
  directStaticOperations + genericStaticOperations,
  395210,
  'the release-artifact memory-operation inventory changed',
)
const result = {
  schema: 1,
  status: performance.status,
  directStaticOperations,
  genericStaticOperations,
  directStaticShare:
    directStaticOperations / (directStaticOperations + genericStaticOperations),
  inferredPrivateParameters: report.inferredPrivateParameters,
  privateReturnExports: report.privateReturnExports,
  worstThroughputRatio: performance.worstThroughputRatio,
  workloadRatios: performance.ratios,
  next:
    performance.status === 'pass'
      ? 'run the complete Phase 2 exit gate'
      : 'rank residual hot generic accesses and add sound hoisting or cloning',
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log(
  `Phase 2B provenance: ${(result.directStaticShare * 100).toFixed(1)}% direct, ${result.worstThroughputRatio.toFixed(2)}x worst case: ${result.status}`,
)

function sum(values) {
  return Object.values(values).reduce((total, value) => total + value, 0)
}
