#!/usr/bin/env node

import { readdir, readFile, writeFile } from 'node:fs/promises'
import { join, resolve } from 'node:path'

const [repoRootArg, outArg, target, statusText] = process.argv.slice(2)
if (statusText === undefined) {
  throw new Error('usage: summarize-phase7.mjs REPO_ROOT OUT TARGET STATUS')
}
const repoRoot = resolve(repoRootArg)
const out = resolve(outArg)
const status = Number.parseInt(statusText, 10)
const provider = join(out, 'provider')
const clusterDirectory = join(out, 'results/clusters')
let clusterFiles = []
try {
  clusterFiles = (await readdir(clusterDirectory)).filter((name) =>
    name.endsWith('.json'),
  )
} catch (error) {
  if (error?.code !== 'ENOENT') throw error
}
const clusters = await Promise.all(
  clusterFiles.map(async (name) =>
    JSON.parse(await readFile(join(clusterDirectory, name), 'utf8')),
  ),
)
const capabilities = JSON.parse(
  await readFile(join(provider, 'capabilities.json'), 'utf8'),
)
const config = JSON.parse(await readFile(join(provider, 'config.json'), 'utf8'))
const capabilityCounts = Object.values(capabilities.capabilities).reduce(
  (counts, capability) => {
    counts[capability.state] = (counts[capability.state] ?? 0) + 1
    return counts
  },
  { SUPPORTED: 0, UNSUPPORTED: 0, BLOCKED: 0 },
)
const peak = clusters.reduce(
  (current, cluster) => ({
    workers: Math.max(current.workers, cluster.peak?.liveProcesses ?? 0),
    rss: Math.max(current.rss, cluster.peak?.rss ?? 0),
    privateMemoryBytes: Math.max(
      current.privateMemoryBytes,
      cluster.peak?.privateMemoryBytes ?? 0,
    ),
    globalMemoryBytes: Math.max(
      current.globalMemoryBytes,
      cluster.peak?.globalMemoryBytes ?? 0,
    ),
  }),
  { workers: 0, rss: 0, privateMemoryBytes: 0, globalMemoryBytes: 0 },
)
const summary = {
  schema: 1,
  status: status === 0 ? 'pass' : 'fail',
  target,
  upstreamExitStatus: status,
  postgresRevision: config.postgresRevision,
  architecture: config.architecture,
  provider,
  canonicalCommand: `PGLITE_TEST_PROVIDER=${provider} make ${target}`,
  capabilityCounts,
  clusters: {
    count: clusters.length,
    passed: clusters.filter((cluster) => cluster.status === 'pass').length,
    failed: clusters.filter((cluster) => cluster.status !== 'pass').length,
  },
  peak,
  preserved: {
    log: join(out, `results/${target}.log`),
    nativeBuild: join(out, 'native/build'),
    clusterResults: clusterDirectory,
  },
  repoRoot,
}
await writeFile(
  join(out, `results/${target}.json`),
  `${JSON.stringify(summary, null, 2)}\n`,
)
console.log(JSON.stringify(summary, null, 2))
