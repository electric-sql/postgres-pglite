import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import {
  mkdtempSync,
  mkdirSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { test } from 'node:test'
import {
  compilerWrapper,
  digest,
  fingerprint,
  loadPolicy,
  planCompilation,
} from '../../tools/memory-placement/cc.mjs'

function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), 'pglite-placement-'))
  t.after(() => rmSync(root, { recursive: true, force: true }))
  const source = 'src/example.c'
  mkdirSync(dirname(join(root, source)), { recursive: true })
  writeFileSync(join(root, source), 'void example(void) {}\n')
  const rule = {
    source,
    sha256: digest(readFileSync(join(root, source))),
    functions: ['dsa_create_ext'],
    reason: 'Area handles stay within the backend group.',
  }
  const policy = { schema: 1, rules: [rule] }
  const path = join(root, 'policy.json')
  writeFileSync(path, JSON.stringify(policy))
  return { root, path, policy, rule }
}

const flags = [
  '-D__PGLITE_POSTMASTER__',
  '-c',
  'src/example.c',
  '-o',
  'example.o',
]

test('selects a reviewed source and leaves unknown sources unchanged', (t) => {
  const { root, path } = fixture(t)
  const policy = loadPolicy(path)
  const plan = planCompilation(flags, policy, root, root)
  assert.ok(plan.args.includes('-DPGLITE_MEMORY_ROOT_DSA=1'))
  assert.ok(!plan.args.includes('-DPGLITE_MEMORY_ROOT_DSM=1'))
  const other = ['-c', 'other/example.c']
  assert.deepEqual(planCompilation(other, policy, root, root), { args: other })
  assert.deepEqual(planCompilation(['a.o', '-o', 'a.so'], policy, root, root), {
    args: ['a.o', '-o', 'a.so'],
  })
})

test('rejects source drift, an incompatible ABI, and mixed source invocations', (t) => {
  const { root, policy } = fixture(t)
  assert.throws(
    () => planCompilation(flags.slice(1), policy, root, root),
    /memory ABI/,
  )
  assert.throws(
    () => planCompilation([...flags, 'other.c'], policy, root, root),
    /one source/,
  )
  writeFileSync(join(root, 'src/example.c'), 'void changed(void) {}\n')
  assert.throws(
    () => planCompilation(flags, policy, root, root),
    /review ownership/,
  )
})

test('rejects conflicting rules and unsupported functions', (t) => {
  const { path, policy, rule } = fixture(t)
  for (const rules of [
    [rule, rule],
    [{ ...rule, functions: ['malloc'] }],
    [{ ...rule, source: '../example.c' }],
    [{ ...rule, functions: [], undefines: ['__PGLITE_POSTMASTER__'] }],
    [{ ...rule, undefines: '__PGLITE__' }],
  ]) {
    writeFileSync(path, JSON.stringify({ ...policy, rules }))
    assert.throws(() => loadPolicy(path), /Invalid memory placement rule/)
  }
})

test('changes the build fingerprint when reviewed policy changes', (t) => {
  const { root, path, policy } = fixture(t)
  const before = fingerprint(path, root)
  policy.rules[0].functions = ['dsm_create']
  writeFileSync(path, JSON.stringify(policy))
  assert.notEqual(fingerprint(path, root), before)
})

test('routes loader metadata only for its reviewed source', (t) => {
  const { root, policy } = fixture(t)
  policy.rules[0].functions = ['stat']
  const plan = planCompilation(flags, policy, root, root)
  assert.ok(plan.args.includes('-DPGLITE_LOADER_STAT=1'))
  assert.ok(!plan.args.includes('-DPGLITE_MEMORY_ROOT_DSM=1'))
})

test('restores reviewed postmaster behavior while preserving classic flags', (t) => {
  const { root, path, policy } = fixture(t)
  policy.rules[0].functions = []
  policy.rules[0].undefines = ['__PGLITE__']
  writeFileSync(path, JSON.stringify(policy))
  const reviewed = loadPolicy(path)
  const args = [...flags, '-D__PGLITE__']
  const plan = planCompilation(args, reviewed, root, root)
  assert.ok(
    plan.args.indexOf('-U__PGLITE__') > plan.args.indexOf('-D__PGLITE__'),
  )
  assert.deepEqual(planCompilation(args, reviewed, root, root, 'classic'), {
    args,
  })
})

test('redirects only reviewed backend executable discovery', (t) => {
  const { root, policy } = fixture(t)
  policy.rules[0].functions = ['find_other_exec']
  assert.ok(
    planCompilation(flags, policy, root, root).args.includes(
      '-DPGLITE_BACKEND_EXECUTABLE=1',
    ),
  )
})

test('uses static frontend encoding APIs in both build modes', (t) => {
  const { root, policy } = fixture(t)
  for (const mode of ['classic', 'postmaster']) {
    for (const directory of [
      'src/bin/psql',
      'src/bin/scripts',
      'src/fe_utils',
      'src/interfaces/libpq',
    ]) {
      const plan = planCompilation(
        ['-c', `${directory}/example.c`],
        policy,
        root,
        root,
        mode,
      )
      assert.ok(
        plan.args.some(
          (flag) =>
            flag.includes('private') || flag === '-DUSE_PRIVATE_ENCODING_FUNCS',
        ),
      )
    }
  }
  assert.deepEqual(
    planCompilation(flags.slice(1), policy, root, root, 'classic'),
    { args: flags.slice(1) },
  )
})

test('generated compiler wrapper preserves arguments and applies the policy', (t) => {
  const { root, path, policy } = fixture(t)
  const realCompiler = join(root, 'fake compiler')
  writeFileSync(
    realCompiler,
    '#!/usr/bin/env node22\nconsole.log(JSON.stringify(process.argv.slice(2)))\n',
    { mode: 0o755 },
  )
  const wrapper = join(root, 'cc')
  writeFileSync(wrapper, compilerWrapper(policy, realCompiler), { mode: 0o755 })
  const env = {
    ...process.env,
    PGLITE_MEMORY_PLACEMENT_ROOT: root,
    PGLITE_MEMORY_PLACEMENT_POLICY: path,
    PGLITE_MEMORY_PLACEMENT_REAL_CC: realCompiler,
  }
  const result = spawnSync(wrapper, [...flags, '-DVALUE=a b;$example'], {
    cwd: root,
    env,
    encoding: 'utf8',
  })
  assert.equal(result.status, 0, result.stderr)
  const received = JSON.parse(result.stdout)
  assert.ok(received.includes('-DVALUE=a b;$example'))
  assert.ok(received.includes('-DPGLITE_MEMORY_ROOT_DSA=1'))
  writeFileSync(
    wrapper,
    compilerWrapper({ schema: 1, rules: [] }, realCompiler),
    { mode: 0o755 },
  )
  const unclassified = spawnSync(wrapper, ['-c', 'unknown.c'], {
    cwd: root,
    env,
    encoding: 'utf8',
  })
  assert.equal(unclassified.status, 0, unclassified.stderr)
  assert.deepEqual(JSON.parse(unclassified.stdout), ['-c', 'unknown.c'])
})
