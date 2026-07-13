#!/usr/bin/env node

import assert from 'node:assert/strict'
import { readFile, writeFile } from 'node:fs/promises'

const [artifactPath, differentialPath, allocationPath, buildPath, outputPath] =
  process.argv.slice(2)
if (!outputPath) {
  throw new Error(
    'usage: phase3-summary.mjs ARTIFACT DIFFERENTIAL ALLOCATION BUILD OUTPUT',
  )
}

const [artifact, differential, allocation, build] = await Promise.all(
  [artifactPath, differentialPath, allocationPath, buildPath].map(
    async (path) => JSON.parse(await readFile(path, 'utf8')),
  ),
)
for (const [name, result] of Object.entries({
  artifact,
  differential,
  allocation,
  build,
})) {
  assert.equal(result.status, 'pass', `${name} result did not pass`)
}
assert.equal(differential.cases, 9)
assert.equal(allocation.sharedPrivate, true)
assert.equal(allocation.sharedGlobal, true)
assert.equal(allocation.distinctDomains, true)
assert.equal(build.dependenciesRebuilt, true)
assert.equal(build.postgresqlRebuilt, true)
assert.equal(build.pthreadRuntime, false)
assert.ok(build.sharedSideModulesAudited > 0)

const result = {
  schema: 1,
  phase: '3-shared-atomics-world',
  status: 'pass',
  build,
  artifact,
  differential,
  syntheticGlobalAllocation: allocation,
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log('PGlite multi-memory Phase 3 shared/atomics gate: PASS')
