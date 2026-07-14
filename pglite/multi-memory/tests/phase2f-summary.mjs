import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { readFile, stat, writeFile } from 'node:fs/promises'

const [
  reportPath,
  classicPath,
  candidatePath,
  differentialPath,
  debugDifferentialPath,
  dynamicPath,
  hostPath,
  packagePath,
  transformPath,
  perf1Path,
  perf2Path,
  perf3Path,
  outputPath,
] = process.argv.slice(2)
if (!outputPath) {
  throw new Error(
    'usage: phase2f-summary.mjs REPORT CLASSIC CANDIDATE DIFFERENTIAL DEBUG_DIFFERENTIAL DYNAMIC HOST PACKAGE TRANSFORM PERF1 PERF2 PERF3 OUTPUT',
  )
}

const [
  report,
  differential,
  debugDifferential,
  dynamic,
  host,
  packageTests,
  transform,
  ...performance
] = await Promise.all(
  [
    reportPath,
    differentialPath,
    debugDifferentialPath,
    dynamicPath,
    hostPath,
    packagePath,
    transformPath,
    perf1Path,
    perf2Path,
    perf3Path,
  ].map(readJSON),
)

const workloadNames = Object.keys(performance[0].ratios)
const conservativeRatios = Object.fromEntries(
  workloadNames.map((name) => [
    name,
    Math.max(...performance.map(({ ratios }) => ratios[name])),
  ]),
)
const worstThroughputRatio = Math.max(...Object.values(conservativeRatios))
const [classic, candidate] = await Promise.all([
  artifact(classicPath),
  artifact(candidatePath),
])
const sum = (values) =>
  Object.values(values).reduce((total, value) => total + value, 0)
const residualGenericFunctions = report.functions
  .map((fn) => ({
    name: fn.name,
    wasmFunctionIndex: fn.wasmFunctionIndex,
    genericOperations: fn.genericOperations,
    genericTotal: sum(fn.genericOperations),
  }))
  .filter(({ genericTotal }) => genericTotal > 0)
  .sort(
    (left, right) =>
      right.genericTotal - left.genericTotal ||
      left.wasmFunctionIndex - right.wasmFunctionIndex,
  )
  .slice(0, 25)

const result = {
  schema: 1,
  status: 'pass',
  threshold: 1.35,
  toolVersion: report.abi.toolVersion,
  repeatedRuns: performance.length,
  worstThroughputRatio,
  conservativeRatios,
  runRatios: performance.map(({ worstThroughputRatio, ratios }) => ({
    worstThroughputRatio,
    ratios,
  })),
  compileMs: {
    classic: performance.map(({ classic }) => classic.compileMs),
    candidate: performance.map(({ transformed }) => transformed.compileMs),
  },
  startupMs: {
    classic: performance.map(({ classic }) => classic.startupMs),
    candidate: performance.map(({ transformed }) => transformed.startupMs),
  },
  transformMs: transform.transformMs,
  differentialCases: differential.cases ?? 0,
  debugDifferentialCases: debugDifferential.cases ?? 0,
  static: {
    direct: report.directPrivate,
    directTotal: sum(report.directPrivate),
    generic: report.rewritten,
    genericTotal: sum(report.rewritten),
    removedPrivateIdentityCalls: report.removedPrivateIdentityCalls,
    inferredPrivateParameters: report.inferredPrivateParameters,
  },
  dynamic,
  residualGenericFunctions,
  hostImports: host,
  packageTests,
  artifacts: {
    classic,
    candidate,
    sizeRatio: candidate.bytes / classic.bytes,
  },
}

assert.equal(report.abi.memoryAccessProfiling, false)
assert.equal(result.toolVersion, '0.11.0')
assert.equal(performance.length, 3)
assert.ok(performance.every(({ status }) => status === 'pass'))
assert.ok(worstThroughputRatio <= result.threshold)
assert.equal(result.differentialCases, 9)
assert.equal(result.debugDifferentialCases, 9)
assert.equal(dynamic.status, 'pass')
assert.ok(dynamic.total.directTotal > dynamic.total.genericTotal)
assert.equal(packageTests.status, 'pass')
assert.equal(host.imports, 136)
assert.equal(host.pointerParameters, 84)
assert.ok(result.static.directTotal > 0)
assert.ok(result.static.genericTotal > 0)

await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log(
  `Phase 2F exit gate: ${worstThroughputRatio.toFixed(3)}x worst, ` +
    `${dynamic.total.directTotal} dynamic direct / ${dynamic.total.genericTotal} generic: PASS`,
)

async function readJSON(path) {
  return JSON.parse(await readFile(path, 'utf8'))
}

async function artifact(path) {
  const [metadata, contents] = await Promise.all([stat(path), readFile(path)])
  return {
    bytes: metadata.size,
    sha256: createHash('sha256').update(contents).digest('hex'),
  }
}
