#!/usr/bin/env node

import assert from 'node:assert/strict'
import { mkdtemp, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { pathToFileURL } from 'node:url'

const [repoRoot, wasm, glue, data, output] = process.argv.slice(2)
if (!output) {
  throw new Error(
    'usage: phase6-correctness.mjs REPO_ROOT WASM GLUE DATA OUTPUT',
  )
}

const dataDirectory = await mkdtemp(join(tmpdir(), 'pglite-phase6-core-'))
let postmaster
const sessions = new Set()

try {
  const { PGlitePostmaster, PostgresProcessKind } = await import(
    pathToFileURL(join(repoRoot, 'packages/pglite/dist/postmaster/index.js'))
      .href
  )
  postmaster = await PGlitePostmaster.create({
    dataDir: `file://${dataDirectory}`,
    maxConnections: 12,
    sharedBuffers: '16MB',
    artifact: { wasm, glue, data },
    startParams: ['-c', 'deadlock_timeout=50ms'],
  })

  const a = await open(postmaster)
  const b = await open(postmaster)
  const control = await open(postmaster)
  const startup = postmaster.diagnostics()
  assert.ok(startup.liveProcesses >= 4, 'auxiliary processes did not start')
  const processKinds = countProcessKinds(postmaster, PostgresProcessKind)
  assert.ok(processKinds.postmaster >= 1, 'postmaster Worker did not start')
  assert.ok(processKinds.auxiliary >= 1, 'auxiliary Workers did not start')
  assert.ok(
    processKinds.backgroundWorker >= 1,
    'built-in background Worker did not start',
  )

  await control.exec(`
    CREATE ROLE phase6_user LOGIN;
    CREATE TABLE phase6_mvcc(id int PRIMARY KEY, value int NOT NULL);
    INSERT INTO phase6_mvcc VALUES (1, 10), (2, 20);
  `)
  const role = await open(postmaster, { username: 'phase6_user' })
  assert.deepEqual(
    (await role.query('SELECT current_user AS role')).rows,
    [{ role: 'phase6_user' }],
  )

  await a.exec("SET application_name='phase6-a'; SET work_mem='1MB'")
  await b.exec("SET application_name='phase6-b'; SET work_mem='2MB'")
  const gucs = await Promise.all([
    a.query(
      "SELECT current_setting('application_name') AS app, current_setting('work_mem') AS work_mem",
    ),
    b.query(
      "SELECT current_setting('application_name') AS app, current_setting('work_mem') AS work_mem",
    ),
  ])
  assert.deepEqual(gucs[0].rows, [{ app: 'phase6-a', work_mem: '1MB' }])
  assert.deepEqual(gucs[1].rows, [{ app: 'phase6-b', work_mem: '2MB' }])

  await a.exec('PREPARE phase6_same(int) AS SELECT $1 + 1 AS value')
  await b.exec('PREPARE phase6_same(int) AS SELECT $1 + 100 AS value')
  assert.deepEqual((await a.query('EXECUTE phase6_same(1)')).rows, [
    { value: 2 },
  ])
  assert.deepEqual((await b.query('EXECUTE phase6_same(1)')).rows, [
    { value: 101 },
  ])

  await a.exec(
    'CREATE TEMP TABLE phase6_temp(value int); INSERT INTO phase6_temp VALUES (7)',
  )
  assert.deepEqual(
    (
      await b.query(
        "SELECT to_regclass('pg_temp.phase6_temp') IS NULL AS isolated",
      )
    ).rows,
    [{ isolated: true }],
  )

  await a.exec('BEGIN; DECLARE phase6_cursor CURSOR FOR SELECT generate_series(1, 3) AS value')
  assert.deepEqual((await a.query('FETCH 2 FROM phase6_cursor')).rows, [
    { value: 1 },
    { value: 2 },
  ])
  assert.deepEqual((await a.query('FETCH ALL FROM phase6_cursor')).rows, [
    { value: 3 },
  ])
  await a.exec('COMMIT')

  await a.exec('BEGIN; UPDATE phase6_mvcc SET value = 11 WHERE id = 1')
  assert.deepEqual(
    (await b.query('SELECT value FROM phase6_mvcc WHERE id = 1')).rows,
    [{ value: 10 }],
  )
  await b.exec("SET lock_timeout='150ms'")
  await expectSqlState(
    b.query('UPDATE phase6_mvcc SET value = 12 WHERE id = 1'),
    '55P03',
  )
  await a.exec('COMMIT')
  assert.deepEqual(
    (await b.query('SELECT value FROM phase6_mvcc WHERE id = 1')).rows,
    [{ value: 11 }],
  )

  await a.exec("SET lock_timeout='0'; BEGIN; UPDATE phase6_mvcc SET value = value WHERE id = 1")
  await b.exec("SET lock_timeout='0'; BEGIN; UPDATE phase6_mvcc SET value = value WHERE id = 2")
  const deadlockA = a.query(
    'UPDATE phase6_mvcc SET value = value WHERE id = 2',
  )
  await delay(75)
  const deadlockB = b.query(
    'UPDATE phase6_mvcc SET value = value WHERE id = 1',
  )
  const deadlockResults = await Promise.allSettled([deadlockA, deadlockB])
  assert.equal(
    deadlockResults.filter(
      (result) => result.status === 'rejected' && sqlState(result.reason) === '40P01',
    ).length,
    1,
    `expected one deadlock victim: ${formatSettled(deadlockResults)}`,
  )
  await Promise.allSettled([a.exec('ROLLBACK'), b.exec('ROLLBACK')])

  await a.query('SELECT pg_advisory_lock(61723)')
  assert.deepEqual(
    (await b.query('SELECT pg_try_advisory_lock(61723) AS acquired')).rows,
    [{ acquired: false }],
  )
  await a.query('SELECT pg_advisory_unlock(61723)')
  assert.deepEqual(
    (await b.query('SELECT pg_try_advisory_lock(61723) AS acquired')).rows,
    [{ acquired: true }],
  )
  await b.query('SELECT pg_advisory_unlock(61723)')

  let resolveNotification
  const notified = new Promise((resolve) => {
    resolveNotification = resolve
  })
  const unlisten = await a.listen('phase6_notify', (payload) =>
    resolveNotification(payload),
  )
  await b.exec("NOTIFY phase6_notify, 'delivered'")
  assert.equal(await withTimeout(notified, 5_000, 'notification'), 'delivered')
  await unlisten()

  await a.exec("SET statement_timeout='100ms'")
  await expectSqlState(a.query('SELECT pg_sleep(5)'), '57014')
  await a.exec("SET statement_timeout='0'")

  const targetPid = (await a.query('SELECT pg_backend_pid() AS pid')).rows[0]
    .pid
  const cancelled = a.query('SELECT pg_sleep(5)')
  const cancellationResult = expectSqlState(cancelled, '57014')
  await delay(100)
  assert.deepEqual(
    (await control.query('SELECT pg_cancel_backend($1) AS cancelled', [targetPid]))
      .rows,
    [{ cancelled: true }],
  )
  await cancellationResult

  const terminated = await open(postmaster)
  const terminatedPid = (
    await terminated.query('SELECT pg_backend_pid() AS pid')
  ).rows[0].pid
  assert.deepEqual(
    (
      await control.query('SELECT pg_terminate_backend($1) AS terminated', [
        terminatedPid,
      ])
    ).rows,
    [{ terminated: true }],
  )
  await assert.rejects(
    () => terminated.query('SELECT 1'),
    /closed|terminat|connection/i,
  )
  await terminated.close().catch(() => undefined)
  sessions.delete(terminated)

  await a.exec('BEGIN; UPDATE phase6_mvcc SET value = 99 WHERE id = 1; ROLLBACK')
  assert.deepEqual(
    (await b.query('SELECT value FROM phase6_mvcc WHERE id = 1')).rows,
    [{ value: 11 }],
  )

  await control.query('SELECT pg_stat_force_next_flush()')
  const statistics = await control.query(`
    SELECT datname, numbackends >= 1 AS has_backends,
           xact_commit > 0 AS has_commits
      FROM pg_stat_database
     WHERE datname = current_database()
  `)
  assert.deepEqual(statistics.rows, [
    { datname: 'postgres', has_backends: true, has_commits: true },
  ])

  const beforeClose = postmaster.diagnostics()
  await Promise.all([...sessions].map((session) => session.close()))
  sessions.clear()
  await waitFor(
    () => postmaster.diagnostics().liveProcesses < beforeClose.liveProcesses,
    10_000,
    'backend cleanup',
  )
  await postmaster.close()
  const shutdown = postmaster.diagnostics()
  assert.equal(shutdown.livePrivateMemories, 0)
  assert.equal(
    shutdown.privateMemoriesStarted,
    shutdown.privateMemoriesReleased,
  )

  await writeFile(
    output,
    `${JSON.stringify(
      {
        schema: 1,
        status: 'pass',
        semantics: [
          'roles',
          'guc-isolation',
          'prepared-statements',
          'temporary-objects',
          'portals',
          'mvcc',
          'lock-timeout',
          'deadlock',
          'advisory-locks',
          'listen-notify',
          'statement-timeout',
          'cancel',
          'terminate',
          'rollback',
          'cumulative-statistics',
        ],
        startup,
        processKinds,
        beforeClose,
        shutdown,
      },
      null,
      2,
    )}\n`,
  )
  console.log('Phase 6 focused multi-session correctness gate: PASS')
} finally {
  await Promise.allSettled([...sessions].map((session) => session.close()))
  await postmaster?.close().catch(() => undefined)
  await rm(dataDirectory, { recursive: true, force: true })
}

async function open(server, options) {
  const deadline = Date.now() + 60_000
  let lastError
  while (Date.now() < deadline) {
    try {
      const session = await server.createSession(options)
      sessions.add(session)
      return session
    } catch (error) {
      lastError = error
      await delay(100)
    }
  }
  throw lastError ?? new Error('postmaster session did not become ready')
}

async function expectSqlState(promise, expected) {
  try {
    await promise
    assert.fail(`expected PostgreSQL error ${expected}`)
  } catch (error) {
    assert.equal(sqlState(error), expected, String(error))
  }
}

function sqlState(error) {
  return error?.code ?? error?.fields?.code
}

function formatSettled(results) {
  return results
    .map((result) =>
      result.status === 'fulfilled'
        ? 'fulfilled'
        : `rejected(${sqlState(result.reason)}: ${String(result.reason)})`,
    )
    .join(', ')
}

async function waitFor(predicate, timeout, label) {
  const deadline = Date.now() + timeout
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error(`${label} timed out`)
    await delay(25)
  }
}

async function withTimeout(promise, timeout, label) {
  let timer
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(`${label} timed out`)), timeout)
      }),
    ])
  } finally {
    clearTimeout(timer)
  }
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds))
}

function countProcessKinds(postmaster, kinds) {
  const counts = {
    postmaster: 0,
    backend: 0,
    auxiliary: 0,
    backgroundWorker: 0,
  }
  for (const handle of postmaster.registry.handles()) {
    const snapshot = postmaster.registry.snapshot(handle)
    if (snapshot.kind === kinds.Postmaster) counts.postmaster++
    else if (snapshot.kind === kinds.Backend) counts.backend++
    else if (snapshot.kind === kinds.Auxiliary) counts.auxiliary++
    else if (snapshot.kind === kinds.BackgroundWorker) {
      counts.backgroundWorker++
    }
  }
  return counts
}
