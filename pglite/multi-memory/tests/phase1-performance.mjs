import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { readFile, stat, writeFile } from 'node:fs/promises'
import { performance } from 'node:perf_hooks'
import { pathToFileURL } from 'node:url'

if (process.argv[2] === '--child') {
  await runChild(...process.argv.slice(3))
} else {
  await runParent(...process.argv.slice(2))
}

async function runParent(
  classicPath,
  transformedPath,
  pgliteEntry,
  outputPath,
  thresholdText = '1.35',
) {
  if (!outputPath) {
    throw new Error(
      'usage: phase1-performance.mjs CLASSIC TRANSFORMED PGLITE_ENTRY OUTPUT',
    )
  }
  const classicRuns = []
  const transformedRuns = []
  // Alternate order to avoid consistently charging one artifact for host
  // warmup or thermal state, then compare medians across isolated processes.
  for (let round = 0; round < 5; round++) {
    if (round % 2 === 0) {
      classicRuns.push(invoke(classicPath, pgliteEntry))
      transformedRuns.push(invoke(transformedPath, pgliteEntry))
    } else {
      transformedRuns.push(invoke(transformedPath, pgliteEntry))
      classicRuns.push(invoke(classicPath, pgliteEntry))
    }
  }
  const classic = aggregate(classicRuns)
  const transformed = aggregate(transformedRuns)
  const ratios = Object.fromEntries(
    Object.keys(classic.workloads).map((name) => [
      name,
      transformed.workloads[name].medianMs / classic.workloads[name].medianMs,
    ]),
  )
  const worstThroughputRatio = Math.max(...Object.values(ratios))
  const threshold = Number(thresholdText)
  assert.ok(Number.isFinite(threshold) && threshold > 0)
  const result = {
    schema: 1,
    runtime: {
      node: process.version,
      arch: process.arch,
      platform: process.platform,
    },
    threshold,
    status: worstThroughputRatio <= threshold ? 'pass' : 'fail',
    worstThroughputRatio,
    ratios,
    classic,
    transformed,
    runs: { classic: classicRuns, transformed: transformedRuns },
  }
  await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
  console.log(
    `Phase 1 throughput: ${worstThroughputRatio.toFixed(2)}x worst case (limit ${threshold.toFixed(2)}x): ${result.status}`,
  )
  if (result.status !== 'pass') process.exitCode = 1

  function invoke(wasmPath, entry) {
    const child = spawnSync(
      process.execPath,
      [import.meta.filename, '--child', wasmPath, entry],
      { encoding: 'utf8', maxBuffer: 10 * 1024 * 1024 },
    )
    if (child.status !== 0) {
      throw new Error(
        `performance child failed for ${wasmPath}\n${child.stdout}\n${child.stderr}`,
      )
    }
    return JSON.parse(child.stdout)
  }

  function aggregate(runs) {
    const workloadNames = Object.keys(runs[0].workloads)
    return {
      bytes: runs[0].bytes,
      compileMs: median(runs.map(({ compileMs }) => compileMs)),
      startupMs: median(runs.map(({ startupMs }) => startupMs)),
      workloads: Object.fromEntries(
        workloadNames.map((name) => [
          name,
          {
            medianMs: median(
              runs.map(({ workloads }) => workloads[name].medianMs),
            ),
          },
        ]),
      ),
    }
  }

  function median(values) {
    return [...values].sort((left, right) => left - right)[
      Math.floor(values.length / 2)
    ]
  }
}

async function runChild(wasmPath, pgliteEntry) {
  assert.ok(pgliteEntry)
  const { PGlite } = await import(pathToFileURL(pgliteEntry))
  const wasm = await readFile(wasmPath)
  const compileStart = performance.now()
  const pgliteWasmModule = await WebAssembly.compile(wasm)
  const compileMs = performance.now() - compileStart
  const startupStart = performance.now()
  const db = await PGlite.create({ pgliteWasmModule })
  const startupMs = performance.now() - startupStart
  try {
    await db.exec(`
      CREATE TABLE phase1_accounts (
        aid integer PRIMARY KEY,
        bid integer NOT NULL,
        balance integer NOT NULL,
        filler text NOT NULL
      );
      INSERT INTO phase1_accounts
        SELECT i, i % 64, (i * 7919) % 100000, repeat('x', 48)
        FROM generate_series(1, 20000) AS i;
      CREATE INDEX phase1_accounts_bid ON phase1_accounts(bid);
      CREATE TABLE phase1_history(delta integer NOT NULL);
      ANALYZE phase1_accounts;
    `)

    const workloads = {}
    workloads.regressionRecursive = await measure(9, 3, () =>
      db.query(`
        WITH RECURSIVE x(n,v) AS (
          VALUES(1,1::bigint)
          UNION ALL
          SELECT n+1,(v*1664525+1013904223)%2147483647
          FROM x WHERE n<25000
        ) SELECT sum(v),avg(v) FROM x
      `),
    )
    workloads.indexedAggregate = await measure(11, 3, () =>
      db.query(`
        SELECT bid, count(*), sum(balance), avg(balance)
        FROM phase1_accounts
        WHERE bid BETWEEN 7 AND 49
        GROUP BY bid ORDER BY bid
      `),
    )
    let account = 1
    workloads.pgbenchTransaction = await measure(15, 4, async () => {
      account = ((account * 48271) % 19999) + 1
      await db.exec(`
        BEGIN;
        UPDATE phase1_accounts SET balance = balance + 1 WHERE aid = ${account};
        SELECT balance FROM phase1_accounts WHERE aid = ${account};
        INSERT INTO phase1_history VALUES (1);
        COMMIT;
      `)
    })

    const file = await stat(wasmPath)
    process.stdout.write(
      JSON.stringify({ bytes: file.size, compileMs, startupMs, workloads }),
    )
  } finally {
    await db.close()
  }
}

async function measure(samples, warmups, operation) {
  for (let index = 0; index < warmups; index++) await operation()
  const values = []
  for (let index = 0; index < samples; index++) {
    const start = performance.now()
    await operation()
    values.push(performance.now() - start)
  }
  values.sort((left, right) => left - right)
  return { medianMs: values[Math.floor(values.length / 2)], samplesMs: values }
}
