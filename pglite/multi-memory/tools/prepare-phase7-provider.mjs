#!/usr/bin/env node

import assert from 'node:assert/strict'
import {
  chmod,
  cp,
  mkdir,
  readFile,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises'
import { dirname, join, resolve } from 'node:path'
import { execFileSync } from 'node:child_process'

const [repoRootArg, phase6Arg, phase7Arg, nativeArg] = process.argv.slice(2)
if (!nativeArg) {
  throw new Error(
    'usage: prepare-phase7-provider.mjs REPO_ROOT PHASE6 PHASE7 NATIVE',
  )
}
const repoRoot = resolve(repoRootArg)
const phase6 = resolve(phase6Arg)
const phase7 = resolve(phase7Arg)
const native = resolve(nativeArg)
const pgRoot = join(repoRoot, 'postgres-pglite')
const source = join(pgRoot, 'pglite/multi-memory/provider')
const provider = join(phase7, 'provider')
const target = process.env.PGLITE_PHASE7_TARGET ?? 'check'
const jobs = Number.parseInt(process.env.PGLITE_PHASE7_JOBS ?? '2', 10)
assert.ok(Number.isInteger(jobs) && jobs > 0, 'invalid Phase 7 job count')
const resultsRoot = join(phase7, 'results', `raw-${target}`)
const revision = execFileSync('git', ['-C', pgRoot, 'rev-parse', 'HEAD'], {
  encoding: 'utf8',
}).trim()

await rm(provider, { recursive: true, force: true })
await mkdir(resultsRoot, { recursive: true })
await cp(source, provider, { recursive: true })
await Promise.all(
  ['initdb', 'postgres', 'pg_ctl', 'pglite-test-capability', 'prove'].map(
    (name) => chmod(join(provider, 'bin', name), 0o755),
  ),
)

const psql = join(native, 'build/src/bin/psql/psql')
await symlink(psql, join(provider, 'bin/psql'))

const capabilities = JSON.parse(
  await readFile(
    join(pgRoot, 'pglite/multi-memory/phase7-capabilities.json'),
    'utf8',
  ),
)
capabilities.postgresRevision = revision
await writeFile(
  join(provider, 'capabilities.json'),
  `${JSON.stringify(capabilities, null, 2)}\n`,
)

const config = {
  schema: 1,
  architecture: process.arch,
  jobs,
  postgresRevision: revision,
  repoRoot,
  artifact: {
    wasm: join(phase6, 'artifact/postmaster.wasm'),
    glue: join(phase6, 'source-build/bin/pglite.js'),
    data: join(phase6, 'source-build/bin/pglite.data'),
  },
  icuArchive: join(repoRoot, 'packages/pglite-icu-full/static/icu.76.tgz'),
  workerFilesystemModule: join(
    pgRoot,
    'pglite/multi-memory/tests/phase6-nodefs-factory.mjs',
  ),
  privateMaximumMemory: 1024 * 1024 * 1024,
  globalMaximumMemory: 1024 * 1024 * 1024,
  resultsRoot,
  capabilityEvents: join(resultsRoot, 'capabilities', 'events'),
  postgresSource: join(native, 'source'),
  postgresBuild: join(native, 'build'),
  mounts: [
    { root: join(phase6, 'icu'), path: '/pglite/icu' },
    { root: phase7, path: phase7 },
    { root: repoRoot, path: repoRoot },
    { root: '/tmp', path: '/tmp' },
  ],
}
for (const path of [
  config.artifact.wasm,
  config.artifact.glue,
  config.artifact.data,
  config.icuArchive,
  config.workerFilesystemModule,
  psql,
]) {
  assert.equal(typeof path, 'string')
}
await writeFile(
  join(provider, 'config.json'),
  `${JSON.stringify(config, null, 2)}\n`,
)
console.log(provider)
