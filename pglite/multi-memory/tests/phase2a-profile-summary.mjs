import { readFile, writeFile } from 'node:fs/promises'

const [
  releaseReportPath,
  namedReportPath,
  cpuProfilePath,
  entryProfilePath,
  outputPath,
  ...diagnosticPaths
] = process.argv.slice(2)

if (!outputPath) {
  throw new Error(
    'usage: phase2a-profile-summary.mjs RELEASE_REPORT NAMED_REPORT CPU_PROFILE ENTRY_PROFILE OUTPUT [DIAGNOSTIC_REPORT:PERFORMANCE ...]',
  )
}

const [releaseReport, namedReport, cpuProfile, entryProfile] =
  await Promise.all(
    [releaseReportPath, namedReportPath, cpuProfilePath, entryProfilePath].map(
      (path) => readFile(path, 'utf8').then(JSON.parse),
    ),
  )

const namedByShape = new Map()
for (const entry of namedReport.functions) {
  const key = shapeKey(entry)
  const candidates = namedByShape.get(key) ?? []
  candidates.push(entry.name)
  namedByShape.set(key, candidates)
}

const entryByIndex = new Map(
  entryProfile.functions.map((entry) => [entry.wasmFunctionIndex, entry]),
)
const releaseByIndex = new Map(
  releaseReport.functions.map((entry) => [entry.wasmFunctionIndex, entry]),
)
const sampleByIndex = new Map(
  cpuProfile.functions.map((entry) => [entry.wasmFunctionIndex, entry]),
)

const indices = new Set([...sampleByIndex.keys(), ...entryByIndex.keys()])
const functions = [...indices]
  .map((wasmFunctionIndex) => {
    const stat = releaseByIndex.get(wasmFunctionIndex)
    if (!stat) {
      throw new Error(`profile refers to unknown function ${wasmFunctionIndex}`)
    }
    const candidates = [...new Set(namedByShape.get(shapeKey(stat)) ?? [])]
    const cpu = sampleByIndex.get(wasmFunctionIndex)
    const entries = entryByIndex.get(wasmFunctionIndex)
    return {
      wasmFunctionIndex,
      symbol: candidates.length === 1 ? candidates[0] : stat.name,
      symbolConfidence:
        candidates.length === 1
          ? 'exact-unique-structural-match'
          : candidates.length
            ? 'ambiguous-structural-match'
            : 'unmapped',
      symbolCandidates: candidates,
      cpuSamples: cpu?.samples ?? 0,
      cpuSampleShare: cpu?.sampleShare ?? 0,
      functionEntries: entries?.calls ?? 0,
      staticMemoryOperations: stat.staticMemoryOperations,
      estimatedStaticAccesses: entries?.estimatedStaticAccesses ?? 0,
      operations: stat.operations,
    }
  })
  .sort(
    (left, right) =>
      right.cpuSamples - left.cpuSamples ||
      right.estimatedStaticAccesses - left.estimatedStaticAccesses,
  )

let cumulativeCpuSampleShare = 0
for (const entry of functions) {
  cumulativeCpuSampleShare += entry.cpuSampleShare
  entry.cumulativeCpuSampleShare = cumulativeCpuSampleShare
}

const diagnostics = []
for (const spec of diagnosticPaths) {
  const separator = spec.indexOf(':')
  if (separator < 1) {
    throw new Error(`diagnostic must be REPORT:PERFORMANCE, got ${spec}`)
  }
  const reportPath = spec.slice(0, separator)
  const performancePath = spec.slice(separator + 1)
  const [report, performance] = await Promise.all(
    [reportPath, performancePath].map((path) =>
      readFile(path, 'utf8').then(JSON.parse),
    ),
  )
  const label =
    performancePath.match(/direct-([^/]+)-performance\.json$/)?.[1] ??
    performancePath
  const classifiedFunctions = report.functions.filter(
    (entry) => entry.accessClassification === 'diagnostic-direct-private',
  ).length
  diagnostics.push({
    label,
    directPrivateFunctions:
      classifiedFunctions || inferredDiagnosticFunctionCount(label),
    directPrivateStaticOperations: sumValues(report.directPrivate),
    genericStaticOperations: sumValues(report.rewritten),
    artifactBytes: performance.transformed.bytes,
    artifactSizeRatio:
      performance.transformed.bytes / performance.classic.bytes,
    worstThroughputRatio: performance.worstThroughputRatio,
    workloadRatios: performance.ratios,
    status: performance.status,
  })
}

const exactSymbols = functions.filter(
  (entry) => entry.symbolConfidence === 'exact-unique-structural-match',
)
const result = {
  schema: 1,
  warning:
    'Structural symbol matching is a profiling aid across separately linked artifacts, not proof that function indices are interchangeable.',
  releaseInputSHA256: releaseReport.abi.inputSHA256,
  namedInputSHA256: namedReport.abi.inputSHA256,
  profile: {
    samplingIntervalMicroseconds: cpuProfile.samplingIntervalMicroseconds,
    processSamples: cpuProfile.totalSamples,
    mainModuleSamples: cpuProfile.mainModuleSamples,
    matchedMainModuleSamples: cpuProfile.matchedFunctionSamples,
    totalFunctionEntries: entryProfile.totalFunctionEntries,
    observedFunctions: entryProfile.functions.length,
  },
  symbolMapping: {
    exactUnique: exactSymbols.length,
    ambiguous: functions.filter(
      (entry) => entry.symbolConfidence === 'ambiguous-structural-match',
    ).length,
    unmapped: functions.filter((entry) => entry.symbolConfidence === 'unmapped')
      .length,
  },
  topCpuFunctions: functions.slice(0, 100),
  diagnostics,
}

await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log(
  `Phase 2A profile: ${exactSymbols.length}/${functions.length} observed functions mapped uniquely; ${cpuProfile.matchedFunctionSamples}/${cpuProfile.mainModuleSamples} main-module CPU samples attributed`,
)
if (diagnostics.length) {
  console.log(
    `Diagnostic lower bound: ${Math.min(...diagnostics.map((entry) => entry.worstThroughputRatio)).toFixed(3)}x; smallest passing direct set: ${diagnostics.filter((entry) => entry.status === 'pass').sort((left, right) => left.directPrivateStaticOperations - right.directPrivateStaticOperations)[0]?.label ?? 'none'}`,
  )
}

function shapeKey(entry) {
  return JSON.stringify([
    entry.expressionShapeHash,
    entry.expressionCount,
    entry.staticMemoryOperations,
    Object.entries(entry.operations).sort(),
  ])
}

function sumValues(object) {
  return Object.values(object).reduce((sum, value) => sum + value, 0)
}

function inferredDiagnosticFunctionCount(label) {
  if (label === 'executed') return entryProfile.functions.length
  const match = /^top-(\d+)$/.exec(label)
  return match ? Number(match[1]) : null
}
