import { readFile, writeFile } from 'node:fs/promises'

const [
  oraclePath1,
  oraclePath2,
  oraclePath3,
  outlinedPath,
  inlinePath,
  outputPath,
] = process.argv.slice(2)
if (!outputPath) {
  throw new Error(
    'usage: phase2a-summary.mjs ORACLE1 ORACLE2 ORACLE3 OUTLINED INLINE OUTPUT',
  )
}

const [oracle1, oracle2, oracle3, outlinedGeneric, inlineGeneric] =
  await Promise.all(
    [oraclePath1, oraclePath2, oraclePath3, outlinedPath, inlinePath].map(
      async (path) => JSON.parse(await readFile(path, 'utf8')),
    ),
  )
const oracleRuns = [oracle1, oracle2, oracle3]
const oracle = aggregateOracle(oracleRuns)
const continuationThreshold = 1.15
const status =
  oracle.worstThroughputRatio <= continuationThreshold ? 'pass' : 'fail'
const result = {
  schema: 1,
  status,
  continuationThreshold,
  profiles: {
    privateOnlyOracle: oracle,
    outlinedGeneric: compact(outlinedGeneric),
    inlineGeneric: compact(inlineGeneric),
  },
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log(
  `Phase 2A continuation gate: oracle ${oracle.worstThroughputRatio.toFixed(2)}x (limit ${continuationThreshold.toFixed(2)}x): ${status}`,
)
if (status !== 'pass') process.exitCode = 1

function compact(performance) {
  return {
    worstThroughputRatio: performance.worstThroughputRatio,
    ratios: performance.ratios,
    bytes: performance.transformed.bytes,
    sizeRatio: performance.transformed.bytes / performance.classic.bytes,
    compileRatio:
      performance.transformed.compileMs / performance.classic.compileMs,
    startupRatio:
      performance.transformed.startupMs / performance.classic.startupMs,
  }
}

function aggregateOracle(runs) {
  const compactRuns = runs.map(compact)
  const workloadNames = Object.keys(compactRuns[0].ratios)
  return {
    worstThroughputRatio: Math.max(
      ...compactRuns.map(({ worstThroughputRatio }) => worstThroughputRatio),
    ),
    ratios: Object.fromEntries(
      workloadNames.map((name) => [
        name,
        Math.max(...compactRuns.map(({ ratios }) => ratios[name])),
      ]),
    ),
    bytes: compactRuns[0].bytes,
    sizeRatio: compactRuns[0].sizeRatio,
    compileRatio: median(compactRuns.map(({ compileRatio }) => compileRatio)),
    startupRatio: median(compactRuns.map(({ startupRatio }) => startupRatio)),
    runs: compactRuns,
  }
}

function median(values) {
  return [...values].sort((left, right) => left - right)[
    Math.floor(values.length / 2)
  ]
}
