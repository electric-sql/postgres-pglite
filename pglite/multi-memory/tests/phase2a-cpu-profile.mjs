import { readFile, writeFile } from 'node:fs/promises'

const [cpuProfilePath, transformReportPath, outputPath] = process.argv.slice(2)
if (!outputPath) {
  throw new Error(
    'usage: phase2a-cpu-profile.mjs CPU_PROFILE TRANSFORM_REPORT OUTPUT',
  )
}

const [cpuProfile, transformReport] = await Promise.all([
  readFile(cpuProfilePath, 'utf8').then(JSON.parse),
  readFile(transformReportPath, 'utf8').then(JSON.parse),
])
const statByIndex = new Map(
  transformReport.functions.map((entry) => [entry.wasmFunctionIndex, entry]),
)
const statByName = new Map(
  transformReport.functions.map((entry) => [entry.name, entry]),
)
const modules = new Map()
for (const node of cpuProfile.nodes) {
  const match = /^wasm-function\[(\d+)\]$/.exec(node.callFrame.functionName)
  const namedStat = statByName.get(node.callFrame.functionName)
  if ((!match && !namedStat) || !node.callFrame.url.startsWith('wasm:'))
    continue
  const module = modules.get(node.callFrame.url) ?? {
    hits: 0,
    functions: new Map(),
  }
  const hits = node.hitCount ?? 0
  const index = match ? Number(match[1]) : namedStat.wasmFunctionIndex
  module.hits += hits
  module.functions.set(index, (module.functions.get(index) ?? 0) + hits)
  modules.set(node.callFrame.url, module)
}
const [mainModuleUrl, mainModule] = [...modules].sort(
  (left, right) => right[1].hits - left[1].hits,
)[0]
const functions = [...mainModule.functions]
  .filter(([index]) => statByIndex.has(index))
  .map(([wasmFunctionIndex, samples]) => {
    const stat = statByIndex.get(wasmFunctionIndex)
    return {
      wasmFunctionIndex,
      name: stat.name,
      samples,
      sampleShare: samples / mainModule.hits,
      staticMemoryOperations: stat.staticMemoryOperations,
      operations: stat.operations,
    }
  })
  .sort((left, right) => right.samples - left.samples)

let cumulative = 0
for (const entry of functions) {
  cumulative += entry.sampleShare
  entry.cumulativeSampleShare = cumulative
}
const result = {
  schema: 1,
  samplingIntervalMicroseconds: 100,
  totalSamples: cpuProfile.samples.length,
  mainModuleUrl,
  mainModuleSamples: mainModule.hits,
  matchedFunctionSamples: functions.reduce(
    (sum, entry) => sum + entry.samples,
    0,
  ),
  functions,
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log(
  `Phase 2A CPU profile: ${result.matchedFunctionSamples}/${result.mainModuleSamples} main-module samples mapped`,
)
