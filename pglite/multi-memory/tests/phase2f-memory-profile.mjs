import assert from 'node:assert/strict'
import { readFile, writeFile } from 'node:fs/promises'
import { pathToFileURL } from 'node:url'

const [wasmPath, pgliteEntry, reportPath, outputPath] = process.argv.slice(2)
if (!outputPath) {
  throw new Error(
    'usage: phase2f-memory-profile.mjs WASM PGLITE_ENTRY REPORT OUTPUT',
  )
}

const report = JSON.parse(await readFile(reportPath, 'utf8'))
const kinds = report.abi.memoryAccessProfileKinds
assert.equal(report.abi.memoryAccessProfiling, true)
assert.ok(Array.isArray(kinds) && kinds.length > 0)

const counts = [
  new Array(kinds.length).fill(0),
  new Array(kinds.length).fill(0),
]
const reset = () => counts.forEach((values) => values.fill(0))
const snapshot = () => {
  const direct = Object.fromEntries(
    kinds.map((kind, index) => [kind, counts[0][index]]),
  )
  const generic = Object.fromEntries(
    kinds.map((kind, index) => [kind, counts[1][index]]),
  )
  return {
    direct,
    generic,
    directTotal: Object.values(direct).reduce((sum, value) => sum + value, 0),
    genericTotal: Object.values(generic).reduce((sum, value) => sum + value, 0),
  }
}

const { PGlite } = await import(pathToFileURL(pgliteEntry))
const pgliteWasmModule = await WebAssembly.compile(await readFile(wasmPath))

// Initializing a fresh data directory creates an internal PGlite instance and
// intentionally removes extensions from it. Seed with the ordinary artifact so
// the instrumented instance never needs that extension-less initialization
// path. Counts are reset after startup, so database creation is not part of any
// measured workload.
const seed = await PGlite.create()
let seedDataDir
try {
  seedDataDir = await seed.dumpDataDir('none')
} finally {
  await seed.close()
}

const profileExtension = {
  name: 'phase2f-memory-access-profile',
  async setup(_pg, emscriptenOpts) {
    const instantiateWasm = emscriptenOpts.instantiateWasm
    assert.equal(typeof instantiateWasm, 'function')
    return {
      emscriptenOpts: {
        ...emscriptenOpts,
        instantiateWasm(imports, successCallback) {
          imports.pglite = {
            ...(imports.pglite ?? {}),
            profile_memory_access(classification, kind) {
              assert.ok(classification === 0 || classification === 1)
              assert.ok(kind >= 0 && kind < kinds.length)
              counts[classification][kind]++
            },
          }
          return instantiateWasm(imports, successCallback)
        },
      },
    }
  },
}

const db = await PGlite.create({
  pgliteWasmModule,
  loadDataDir: seedDataDir,
  extensions: { profile: profileExtension },
})
const workloads = {}
try {
  reset()
  await db.exec(`
    CREATE TABLE phase2f_accounts (
      aid integer PRIMARY KEY,
      bid integer NOT NULL,
      balance integer NOT NULL,
      filler text NOT NULL
    );
    INSERT INTO phase2f_accounts
      SELECT i, i % 64, (i * 7919) % 100000, repeat('x', 48)
      FROM generate_series(1, 20000) AS i;
    CREATE INDEX phase2f_accounts_bid ON phase2f_accounts(bid);
    CREATE TABLE phase2f_history(delta integer NOT NULL);
    ANALYZE phase2f_accounts;
  `)
  workloads.setup = snapshot()

  reset()
  await db.query(`
    WITH RECURSIVE x(n,v) AS (
      VALUES(1,1::bigint)
      UNION ALL
      SELECT n+1,(v*1664525+1013904223)%2147483647
      FROM x WHERE n<25000
    ) SELECT sum(v),avg(v) FROM x
  `)
  workloads.regressionRecursive = snapshot()

  reset()
  await db.query(`
    SELECT bid, count(*), sum(balance), avg(balance)
    FROM phase2f_accounts
    WHERE bid BETWEEN 7 AND 49
    GROUP BY bid ORDER BY bid
  `)
  workloads.indexedAggregate = snapshot()

  reset()
  for (let account = 1; account <= 15; account++) {
    await db.exec(`
      BEGIN;
      UPDATE phase2f_accounts SET balance = balance + 1 WHERE aid = ${account};
      SELECT balance FROM phase2f_accounts WHERE aid = ${account};
      INSERT INTO phase2f_history VALUES (1);
      COMMIT;
    `)
  }
  workloads.pgbenchTransaction = snapshot()
} finally {
  await db.close()
}

const total = {
  directTotal: Object.values(workloads).reduce(
    (sum, workload) => sum + workload.directTotal,
    0,
  ),
  genericTotal: Object.values(workloads).reduce(
    (sum, workload) => sum + workload.genericTotal,
    0,
  ),
}
const result = {
  schema: 1,
  status: total.directTotal > 0 && total.genericTotal > 0 ? 'pass' : 'fail',
  kinds,
  workloads,
  total,
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
if (result.status !== 'pass') {
  throw new Error(`invalid dynamic memory profile: ${JSON.stringify(total)}`)
}
console.log(
  `Phase 2F dynamic accesses: ${total.directTotal} direct, ${total.genericTotal} generic`,
)
