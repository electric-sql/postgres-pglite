#!/usr/bin/env node22
import { createHash } from 'node:crypto'
import { spawnSync } from 'node:child_process'
import { chmodSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { basename, dirname, relative, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const directory = dirname(fileURLToPath(import.meta.url))
const defaultRoot = resolve(directory, '../../..')
const supported = new Set([
  'dsm_create',
  'dsa_create_ext',
  'stat',
  'sigaction',
  'find_other_exec',
])
const frontendFlags = new Map([
  ['src/bin/pg_dump', ['-Dpg_encoding_to_char=pg_encoding_to_char_private']],
  [
    'src/bin/pg_walsummary',
    ['-Dpg_encoding_to_char=pg_encoding_to_char_private'],
  ],
  ['src/bin/psql', ['-Dpg_encoding_to_char=pg_encoding_to_char_private']],
  ['src/bin/scripts', ['-Dpg_char_to_encoding=pg_char_to_encoding_private']],
  [
    'src/fe_utils',
    [
      '-Dpg_char_to_encoding=pg_char_to_encoding_private',
      '-Dpg_valid_server_encoding_id=pg_valid_server_encoding_id_private',
    ],
  ],
  ['src/interfaces/libpq', ['-DUSE_PRIVATE_ENCODING_FUNCS']],
])
export const digest = (data) => createHash('sha256').update(data).digest('hex')

export function loadPolicy(path) {
  const policy = JSON.parse(readFileSync(path, 'utf8'))
  if (policy.schema !== 1 || !Array.isArray(policy.rules)) {
    throw new Error('Unsupported memory placement policy')
  }
  const seen = new Set()
  for (const rule of policy.rules) {
    if (
      typeof rule.source !== 'string' ||
      rule.source.startsWith('/') ||
      rule.source.split('/').includes('..') ||
      !rule.source.endsWith('.c') ||
      seen.has(rule.source) ||
      !/^[a-f0-9]{64}$/.test(rule.sha256) ||
      !Array.isArray(rule.functions) ||
      (rule.functions.length === 0 && !rule.undefines?.length) ||
      rule.functions.some((name) => !supported.has(name)) ||
      (rule.undefines !== undefined &&
        (!Array.isArray(rule.undefines) ||
          rule.undefines.some((name) => name !== '__PGLITE__'))) ||
      typeof rule.reason !== 'string' ||
      !rule.reason.trim()
    ) {
      throw new Error(`Invalid memory placement rule: ${rule.source}`)
    }
    seen.add(rule.source)
  }
  return policy
}

export function planCompilation(
  args,
  policy,
  root,
  cwd = process.cwd(),
  mode = 'postmaster',
) {
  const sources = args.filter(
    (arg) => !arg.startsWith('-') && arg.endsWith('.c'),
  )
  const flags = sources.flatMap((source) => {
    const directory = relative(root, dirname(resolve(cwd, source)))
    return frontendFlags.get(directory) ?? []
  })
  if (flags.length) args = [...args, ...new Set(flags)]
  if (mode === 'classic') return { args }
  const matches = sources.flatMap((source) => {
    const path = resolve(cwd, source)
    const rule = policy.rules.find(
      (entry) => entry.source === relative(root, path),
    )
    return rule ? [{ rule, path }] : []
  })
  if (matches.length === 0) return { args }
  if (sources.length !== 1)
    throw new Error('Placement requires one source per compiler invocation')
  if (!args.some((arg) => /^-D__PGLITE_POSTMASTER__(=1)?$/.test(arg))) {
    throw new Error('Root placement requires the postmaster memory ABI')
  }
  const { rule, path } = matches[0]
  if (rule.functions.includes('sigaction') && args.includes('-DFRONTEND'))
    return { args }
  if (digest(readFileSync(path)) !== rule.sha256) {
    throw new Error(
      `Memory placement source changed; review ownership: ${rule.source}`,
    )
  }
  const definitions = rule.functions.map((name) =>
    name === 'dsm_create'
      ? '-DPGLITE_MEMORY_ROOT_DSM=1'
      : name === 'dsa_create_ext'
        ? '-DPGLITE_MEMORY_ROOT_DSA=1'
        : name === 'stat'
          ? '-DPGLITE_LOADER_STAT=1'
          : name === 'sigaction'
            ? '-DPGLITE_BACKEND_SIGNALS=1'
            : '-DPGLITE_BACKEND_EXECUTABLE=1',
  )
  return {
    args: [
      ...args,
      ...(rule.undefines ?? []).map((name) => `-U${name}`),
      ...definitions,
      '-include',
      resolve(directory, 'root-placement.h'),
    ],
    rule,
  }
}

export function fingerprint(policyPath, root) {
  const policy = loadPolicy(policyPath)
  for (const rule of policy.rules) {
    if (digest(readFileSync(resolve(root, rule.source))) !== rule.sha256) {
      throw new Error(
        `Memory placement source changed; review ownership: ${rule.source}`,
      )
    }
  }
  return digest(
    Buffer.concat([
      readFileSync(policyPath),
      readFileSync(fileURLToPath(import.meta.url)),
      readFileSync(resolve(directory, 'root-placement.h')),
      readFileSync(
        resolve(defaultRoot, 'pglite/src/pglitec/memory-placement.c'),
      ),
      readFileSync(resolve(defaultRoot, 'pglite/src/pglitec/pglitec.h')),
      readFileSync(
        resolve(defaultRoot, 'pglite/src/pglitec/pglite_backend_launch.h'),
      ),
      readFileSync(
        resolve(defaultRoot, 'pglite/src/postgres-headers/miscadmin.h'),
      ),
      readFileSync(
        resolve(defaultRoot, 'pglite/src/pglitec/pglite_provenance.h'),
      ),
      readFileSync(
        resolve(defaultRoot, 'pglite/src/pglitec/pglite_exec_expr.h'),
      ),
      readFileSync(
        resolve(defaultRoot, 'pglite/src/pglitec/pglite_tuple_deform.h'),
      ),
      readFileSync(
        resolve(defaultRoot, 'pglite/src/pglitec/pglite_tuple_access.h'),
      ),
      readFileSync(resolve(defaultRoot, 'pglite/src/pglitec/semaphore.c')),
      readFileSync(
        resolve(defaultRoot, 'pglite/src/pglitec/directory-entries.js'),
      ),
      readFileSync(resolve(defaultRoot, 'pglite/build-rules.mk')),
    ]),
  )
}

const shellQuote = (value) => `'${value.replaceAll("'", "'\\''")}'`
export function compilerWrapper(
  policy,
  realCompiler,
  node = process.execPath,
  root = defaultRoot,
) {
  const patterns = [
    ...new Set(policy.rules.map((rule) => basename(rule.source))),
  ]
    .flatMap((name) => [shellQuote(name), `*/${shellQuote(name)}`])
    .join('|')
  const driver = `exec ${shellQuote(node)} ${shellQuote(fileURLToPath(import.meta.url))} "$@"`
  const directories = [...frontendFlags.keys()]
    .map((path) => shellQuote(resolve(root, path)))
    .join('|')
  return `#!/bin/sh\ncase "$PWD" in\n  ${directories}) ${driver} ;;\nesac\n${patterns ? `for argument do\n  case "$argument" in\n    ${patterns}) ${driver} ;;\n  esac\ndone\n` : ''}exec ${shellQuote(realCompiler)} "$@"\n`
}

function main() {
  const root = resolve(process.env.PGLITE_MEMORY_PLACEMENT_ROOT ?? defaultRoot)
  const policyPath = resolve(
    process.env.PGLITE_MEMORY_PLACEMENT_POLICY ??
      resolve(directory, 'policy.json'),
  )
  const policy = loadPolicy(policyPath)
  const args = process.argv.slice(2)
  const realCompiler = process.env.PGLITE_MEMORY_PLACEMENT_REAL_CC ?? 'emcc'
  if (args[0] === '--pglite-fingerprint') {
    console.log(fingerprint(policyPath, root))
    return
  }
  if (args[0] === '--pglite-wrapper') {
    fingerprint(policyPath, root)
    const output = resolve(args[1])
    mkdirSync(dirname(output), { recursive: true })
    writeFileSync(
      output,
      compilerWrapper(policy, realCompiler, process.execPath, root),
    )
    chmodSync(output, 0o755)
    return
  }
  const plan = planCompilation(
    args,
    policy,
    root,
    process.cwd(),
    process.env.PGLITE_MEMORY_PLACEMENT_MODE,
  )
  const result = spawnSync(realCompiler, plan.args, { stdio: 'inherit' })
  if (result.error) throw result.error
  if (result.status !== 0) process.exit(result.status ?? 1)
  if (plan.rule && process.env.PGLITE_MEMORY_PLACEMENT_REPORT_DIR) {
    const reportRoot = process.env.PGLITE_MEMORY_PLACEMENT_REPORT_DIR
    mkdirSync(reportRoot, { recursive: true })
    const outputIndex = args.indexOf('-o')
    const output = outputIndex < 0 ? '' : resolve(args[outputIndex + 1])
    writeFileSync(
      resolve(reportRoot, `${digest(output).slice(0, 16)}.json`),
      `${JSON.stringify({ ...plan.rule, output, status: 'applied', policyFingerprint: fingerprint(policyPath, root) }, null, 2)}\n`,
    )
  }
}

if (
  process.argv[1] &&
  resolve(process.argv[1]) === fileURLToPath(import.meta.url)
) {
  try {
    main()
  } catch (error) {
    console.error(error.message)
    process.exitCode = 1
  }
}
