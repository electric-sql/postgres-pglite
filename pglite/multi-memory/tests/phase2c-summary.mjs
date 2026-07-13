import { createHash } from 'node:crypto'
import { readFile, stat, writeFile } from 'node:fs/promises'

const [reportPath, classicPath, candidatePath, differentialPath, ...tail] =
  process.argv.slice(2)
const outputPath = tail.pop()
if (!outputPath || tail.length !== 3) {
  throw new Error(
    'usage: phase2c-summary.mjs REPORT CLASSIC CANDIDATE DIFFERENTIAL PERF1 PERF2 PERF3 OUTPUT',
  )
}

const [report, differential, ...performance] = await Promise.all([
  readJSON(reportPath),
  readJSON(differentialPath),
  ...tail.map(readJSON),
])
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
const result = {
  schema: 1,
  status:
    performance.every(({ status }) => status === 'pass') &&
    worstThroughputRatio <= 1.35
      ? 'pass'
      : 'fail',
  threshold: 1.35,
  repeatedRuns: performance.length,
  worstThroughputRatio,
  conservativeRatios,
  runRatios: performance.map(({ worstThroughputRatio, ratios }) => ({
    worstThroughputRatio,
    ratios,
  })),
  differentialCases: differential.cases?.length ?? 9,
  directPrivate: report.directPrivate,
  generic: report.rewritten,
  removedPrivateIdentityCalls: report.removedPrivateIdentityCalls,
  inferredPrivateParameters: report.inferredPrivateParameters,
  artifacts: {
    classic,
    candidate,
    sizeRatio: candidate.bytes / classic.bytes,
  },
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
if (result.status !== 'pass') {
  throw new Error(`Phase 2C performance gate failed: ${worstThroughputRatio}`)
}

async function readJSON(path) {
  return JSON.parse(await readFile(path, 'utf8'))
}

async function artifact(path) {
  const [bytes, contents] = await Promise.all([stat(path), readFile(path)])
  return {
    bytes: bytes.size,
    sha256: createHash('sha256').update(contents).digest('hex'),
  }
}
