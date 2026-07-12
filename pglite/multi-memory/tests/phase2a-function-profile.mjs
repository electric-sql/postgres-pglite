import { readFile, writeFile } from 'node:fs/promises'
import { pathToFileURL } from 'node:url'

const [wasmPath, pgliteEntry, transformReportPath, outputPath] =
  process.argv.slice(2)
if (!outputPath) {
  throw new Error(
    'usage: phase2a-function-profile.mjs WASM PGLITE_ENTRY REPORT OUTPUT',
  )
}

const { PGlite } = await import(pathToFileURL(pgliteEntry))
const pgliteWasmModule = await WebAssembly.compile(await readFile(wasmPath))
const transformReport = JSON.parse(await readFile(transformReportPath, 'utf8'))
const functionByIndex = new Map(
  transformReport.functions.map((entry) => [entry.wasmFunctionIndex, entry]),
)
let enabled = false
let activeCounts = new Map()

const profileExtension = {
  name: 'multi_memory_function_profile',
  async setup(_pg, emscriptenOpts) {
    const instantiateWasm = emscriptenOpts.instantiateWasm
    return {
      emscriptenOpts: {
        ...emscriptenOpts,
        instantiateWasm(imports, successCallback) {
          imports.pglite = {
            ...(imports.pglite ?? {}),
            profile_function_entry(index) {
              if (!enabled) return
              activeCounts.set(index, (activeCounts.get(index) ?? 0) + 1)
            },
          }
          return instantiateWasm(imports, successCallback)
        },
      },
    }
  },
}

// The profiled main artifact has an extra import. Seed PGDATA with the classic
// artifact so PGlite's internal initdb instance does not need that hook.
const seed = await PGlite.create()
const loadDataDir = await seed.dumpDataDir('none')
await seed.close()
const db = await PGlite.create({
  pgliteWasmModule,
  loadDataDir,
  extensions: { multi_memory_function_profile: profileExtension },
})
const workloads = {}
try {
  await db.exec(`
    CREATE TABLE phase2_profile_accounts (
      aid integer PRIMARY KEY,
      bid integer NOT NULL,
      balance integer NOT NULL,
      filler text NOT NULL
    );
    INSERT INTO phase2_profile_accounts
      SELECT i, i % 64, (i * 7919) % 100000, repeat('x', 48)
      FROM generate_series(1, 20000) AS i;
    CREATE INDEX phase2_profile_accounts_bid
      ON phase2_profile_accounts(bid);
    CREATE TABLE phase2_profile_history(delta integer NOT NULL);
    ANALYZE phase2_profile_accounts;
  `)

  workloads.regressionRecursive = await capture(async () => {
    await db.query(`
      WITH RECURSIVE x(n,v) AS (
        VALUES(1,1::bigint)
        UNION ALL
        SELECT n+1,(v*1664525+1013904223)%2147483647
        FROM x WHERE n<25000
      ) SELECT sum(v),avg(v) FROM x
    `)
  })
  workloads.indexedAggregate = await capture(async () => {
    for (let index = 0; index < 5; index++) {
      await db.query(`
        SELECT bid, count(*), sum(balance), avg(balance)
        FROM phase2_profile_accounts
        WHERE bid BETWEEN 7 AND 49
        GROUP BY bid ORDER BY bid
      `)
    }
  })
  workloads.pgbenchTransaction = await capture(async () => {
    let account = 1
    for (let index = 0; index < 50; index++) {
      account = ((account * 48271) % 19999) + 1
      await db.exec(`
        BEGIN;
        UPDATE phase2_profile_accounts
          SET balance = balance + 1 WHERE aid = ${account};
        SELECT balance FROM phase2_profile_accounts WHERE aid = ${account};
        INSERT INTO phase2_profile_history VALUES (1);
        COMMIT;
      `)
    }
  })
} finally {
  enabled = false
  await db.close()
}

const combined = new Map()
for (const workload of Object.values(workloads)) {
  for (const entry of workload.functions) {
    combined.set(
      entry.wasmFunctionIndex,
      (combined.get(entry.wasmFunctionIndex) ?? 0) + entry.calls,
    )
  }
}
const functions = rank(combined)
const result = {
  schema: 1,
  profile: transformReport.abi.profile,
  totalInstrumentedFunctions: transformReport.functions.length,
  totalFunctionEntries: functions.reduce((sum, entry) => sum + entry.calls, 0),
  estimatedStaticAccesses: functions.reduce(
    (sum, entry) => sum + entry.estimatedStaticAccesses,
    0,
  ),
  functions,
  workloads,
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log(
  `Phase 2A function profile: ${result.totalFunctionEntries} entries across ${functions.length} functions`,
)

async function capture(operation) {
  activeCounts = new Map()
  enabled = true
  try {
    await operation()
  } finally {
    enabled = false
  }
  const functions = rank(activeCounts)
  return {
    totalFunctionEntries: functions.reduce(
      (sum, entry) => sum + entry.calls,
      0,
    ),
    estimatedStaticAccesses: functions.reduce(
      (sum, entry) => sum + entry.estimatedStaticAccesses,
      0,
    ),
    functions,
  }
}

function rank(counts) {
  return [...counts]
    .map(([wasmFunctionIndex, calls]) => {
      const stat = functionByIndex.get(wasmFunctionIndex)
      if (!stat) {
        throw new Error(
          `profile hook reported unknown function ${wasmFunctionIndex}`,
        )
      }
      return {
        wasmFunctionIndex,
        name: stat.name,
        calls,
        staticMemoryOperations: stat.staticMemoryOperations,
        estimatedStaticAccesses: calls * stat.staticMemoryOperations,
        operations: stat.operations,
      }
    })
    .sort(
      (left, right) =>
        right.estimatedStaticAccesses - left.estimatedStaticAccesses ||
        right.calls - left.calls,
    )
}
