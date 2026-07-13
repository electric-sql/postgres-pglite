#!/usr/bin/env node

import assert from 'node:assert/strict'
import { readFile, writeFile } from 'node:fs/promises'

const [
  artifactPath,
  differentialPath,
  allocationPath,
  buildPath,
  regressionPath,
  outputPath,
] = process.argv.slice(2)
if (!outputPath) {
  throw new Error(
    'usage: phase3-summary.mjs ARTIFACT DIFFERENTIAL ALLOCATION BUILD REGRESSION OUTPUT',
  )
}

const [artifact, differential, allocation, build, regression] =
  await Promise.all(
    [
      artifactPath,
      differentialPath,
      allocationPath,
      buildPath,
      regressionPath,
    ].map(async (path) => JSON.parse(await readFile(path, 'utf8'))),
  )
for (const [name, result] of Object.entries({
  artifact,
  differential,
  allocation,
  build,
  regression,
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
assert.equal(build.regressionSideModule, true)
assert.equal(regression.profile, 'phase3-reduced-concurrency')
assert.equal(regression.scope, 'single-backend-no-streaming-copy')
assert.ok(regression.tests.includes('test_setup'))
assert.ok(regression.testCount >= 12)

const result = {
  schema: 1,
  phase: '3-shared-atomics-world',
  status: 'pass',
  build,
  artifact,
  differential,
  syntheticGlobalAllocation: allocation,
  regression,
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log('PGlite multi-memory Phase 3 shared/atomics gate: PASS')
