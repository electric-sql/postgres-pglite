import assert from 'node:assert/strict'
import { readFile, writeFile } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { pathToFileURL } from 'node:url'

const [classicPath, transformedPath, pgliteEntry, outputPath] =
  process.argv.slice(2)
if (!outputPath) {
  throw new Error(
    'usage: phase1-differential.mjs CLASSIC TRANSFORMED PGLITE_ENTRY OUTPUT',
  )
}

const { PGlite } = await import(pathToFileURL(pgliteEntry))
const { pgcrypto } = await import(
  pathToFileURL(join(dirname(pgliteEntry), 'contrib/pgcrypto.js'))
)
const classicModule = await WebAssembly.compile(await readFile(classicPath))
const transformedModule = await WebAssembly.compile(
  await readFile(transformedPath),
)

const classic = await exercise(PGlite, classicModule, pgcrypto)
const transformed = await exercise(PGlite, transformedModule, pgcrypto)
assert.deepEqual(transformed, classic)

const result = {
  schema: 1,
  cases: classic.length,
  names: classic.map(({ name }) => name),
  status: 'pass',
}
await writeFile(outputPath, `${JSON.stringify(result, null, 2)}\n`)
console.log(`Phase 1 differential SQL: ${classic.length} cases passed`)

async function exercise(PGliteClass, pgliteWasmModule, pgcryptoExtension) {
  const db = await PGliteClass.create({
    pgliteWasmModule,
    extensions: { pgcrypto: pgcryptoExtension },
  })
  const results = []
  try {
    await record('ddl-dml-returning', async () => {
      await db.exec(`
        CREATE TABLE accounts (
          id integer PRIMARY KEY,
          balance bigint NOT NULL,
          tags text[] NOT NULL DEFAULT '{}'
        );
        INSERT INTO accounts VALUES
          (1, 1000, ARRAY['one', 'odd']),
          (2, 2500, ARRAY['two', 'even']),
          (3, 4000, ARRAY['three', 'odd']);
        UPDATE accounts SET balance = balance + 17 WHERE id IN (1, 3);
      `)
      return db.query('SELECT id, balance, tags FROM accounts ORDER BY id')
    })

    await record('transaction-rollback', async () => {
      try {
        await db.transaction(async (tx) => {
          await tx.query('UPDATE accounts SET balance = 0')
          throw new Error('deliberate rollback')
        })
      } catch (error) {
        assert.equal(error.message, 'deliberate rollback')
      }
      return db.query('SELECT sum(balance) AS total FROM accounts')
    })

    await record('types-json-arrays-bytea', () =>
      db.query(`
        SELECT
          9223372036854775806::bigint AS i8,
          1234.5678::numeric AS numeric,
          jsonb_build_object('b', 2, 'a', ARRAY[1,2,3]) AS document,
          ARRAY[NULL, 'x', 'é']::text[] AS strings,
          decode('00ff10', 'hex') AS bytes,
          '550e8400-e29b-41d4-a716-446655440000'::uuid AS uuid
      `),
    )

    await record('recursive-regression', () =>
      db.query(`
        WITH RECURSIVE tree(n, path) AS (
          VALUES (1, ARRAY[1])
          UNION ALL
          SELECT n + 1, path || (n + 1) FROM tree WHERE n < 250
        )
        SELECT n, path[1] AS first, path[array_length(path, 1)] AS last
        FROM tree WHERE n IN (1, 125, 250) ORDER BY n
      `),
    )

    await record('planner-index-join', async () => {
      await db.exec(`
        CREATE INDEX accounts_balance_idx ON accounts(balance);
        ANALYZE accounts;
      `)
      return db.query(`
        SELECT a.id, b.id AS peer
        FROM accounts a JOIN accounts b ON a.id < b.id
        WHERE a.balance < b.balance ORDER BY a.id, b.id
      `)
    })

    await record('pg-regress-bool-input', () =>
      db.query(`
        SELECT bool 't' AS t, bool 'off' AS f,
               pg_input_is_valid('yeah', 'bool') AS invalid
      `),
    )

    await record('pg-regress-uuid-operators', () =>
      db.query(`
        SELECT
          '11111111-1111-1111-1111-111111111111'::uuid
            < '22222222-2222-2222-2222-222222222222'::uuid AS lt,
          pg_input_is_valid('11', 'uuid') AS invalid
      `),
    )

    await record('extension-pgcrypto', async () => {
      await db.exec('CREATE EXTENSION pgcrypto')
      return db.query(
        "SELECT encode(digest('pglite', 'sha256'), 'hex') AS digest",
      )
    })

    await record('error-shape', async () => {
      try {
        await db.query('SELECT * FROM phase1_missing_relation')
      } catch (error) {
        return {
          name: error.name,
          code: error.code,
          severity: error.severity,
          message: error.message,
        }
      }
      throw new Error('expected missing-relation query to fail')
    })
  } finally {
    await db.close()
  }
  return results

  async function record(name, operation) {
    results.push({ name, value: normalize(await operation()) })
  }
}

function normalize(value) {
  return JSON.parse(
    JSON.stringify(value, (_, item) => {
      if (typeof item === 'bigint') return { $bigint: item.toString() }
      if (item instanceof Uint8Array) return { $bytes: [...item] }
      return item
    }),
  )
}
