#!/usr/bin/env node

import assert from 'node:assert/strict'
import {
  closeSync,
  existsSync,
  openSync,
  readFileSync,
  realpathSync,
} from 'node:fs'
import {
  mkdir,
  readFile,
  rename,
  rm,
  writeFile,
} from 'node:fs/promises'
import { basename, dirname, join, resolve } from 'node:path'
import { spawn } from 'node:child_process'
import { fileURLToPath, pathToFileURL } from 'node:url'

const PROVIDER_SCHEMA = 1
const STATE_FILE = '.pglite-provider.json'
const VERSION = '18.3'
const [command, ...commandArgs] = process.argv.slice(2)

if (!command) fail('provider command is required')

const providerRoot = resolve(
  process.env.PGLITE_TEST_PROVIDER ??
    join(dirname(fileURLToPath(import.meta.url)), '..'),
)
const config = JSON.parse(
  await readFile(join(providerRoot, 'config.json'), 'utf8'),
)
validateConfig(config)

try {
  if (command === 'initdb') await runInitdb(commandArgs)
  else if (command === 'postgres') await runPostgres(commandArgs)
  else if (command === 'pg_ctl') await runPgCtl(commandArgs)
  else fail(`unsupported provider command: ${command}`)
} catch (error) {
  console.error(error instanceof Error ? error.message : String(error))
  process.exitCode = 1
}

async function runInitdb(args) {
  if (printVersionOrHelp('initdb', args)) return
  const parsed = parseInitdb(args)
  const pgdata = canonicalDataDirectory(parsed.pgdata)
  if (existsSync(join(pgdata, 'PG_VERSION'))) {
    fail(`initdb: directory already contains a database system: ${pgdata}`)
  }
  await mkdir(pgdata, { recursive: true })

  const { PGlite } = await import(
    pathToFileURL(join(config.repoRoot, 'packages/pglite/dist/index.js')).href
  )
  const icuArchive = await readFile(config.icuArchive)
  const database = await PGlite.create({
    dataDir: `file://${pgdata}`,
    icuDataDir: new Blob([icuArchive]),
    initDbStartParams: parsed.initdbArgs,
  })
  await database.close()
  if (!existsSync(join(pgdata, 'PG_VERSION'))) {
    fail(`initdb: PGlite did not initialize ${pgdata}`)
  }
  console.log(`PGlite PostgreSQL data directory initialized at ${pgdata}`)
}

async function runPostgres(args) {
  if (printVersionOrHelp('postgres', args)) return
  const parsed = parsePostgres(args)
  const pgdata = canonicalDataDirectory(parsed.pgdata)
  if (!existsSync(join(pgdata, 'PG_VERSION'))) {
    fail(`postgres: data directory is not initialized: ${pgdata}`)
  }
  await removeStaleLifecycle(pgdata)

  const fileSettings = readPostgresqlSettings(
    join(pgdata, 'postgresql.conf'),
  )
  const setting = (name, fallback) =>
    parsed.settings.get(name) ?? fileSettings.get(name) ?? fallback
  const port = parsePort(setting('port', process.env.PGPORT ?? '5432'))
  const socketDirectories = splitSettingList(
    setting('unix_socket_directories', ''),
  )
  const listenAddresses = splitSettingList(
    setting('listen_addresses', '127.0.0.1'),
  )
  const listen = socketDirectories.length
    ? { directory: socketDirectories[0], port }
    : { host: listenAddresses.find(Boolean) ?? '127.0.0.1', port }
  const configuredConnections = Number.parseInt(
    setting('max_connections', '100'),
    10,
  )
  const maxConnections = Math.max(
    32,
    Number.isInteger(configuredConnections) ? configuredConnections : 100,
  )

  const { PGlitePostmaster } = await import(
    pathToFileURL(
      join(config.repoRoot, 'packages/pglite/dist/postmaster/index.js'),
    ).href
  )
  const { PGliteSocketServer } = await import(
    pathToFileURL(
      join(config.repoRoot, 'packages/pglite-socket/dist/index.js'),
    ).href
  )
  const icuArchive = await readFile(config.icuArchive)
  const postmaster = await PGlitePostmaster.create({
    dataDir: `file://${pgdata}`,
    initialize: false,
    postmasterPid: process.pid,
    maxConnections,
    respectPostgresqlConfig: true,
    icuDataDir: new Blob([icuArchive]),
    artifact: config.artifact,
    startParams: parsed.startParams,
    privateMaximumMemory: config.privateMaximumMemory,
    globalMaximumMemory: config.globalMaximumMemory,
    workerFilesystem: {
      module: config.workerFilesystemModule,
      options: {
        root: pgdata,
        mounts: config.mounts,
      },
    },
    debug: process.env.PGLITE_PROVIDER_DEBUG === 'true',
  })
  const socket = new PGliteSocketServer({ postmaster, listen })
  const startedAt = Date.now()
  let peak = sample(postmaster)
  const sampler = setInterval(() => {
    peak = maximumSample(peak, sample(postmaster))
  }, 100)
  let requestedMode
  let shutdownPromise

  const shutdown = (mode, reason) => {
    requestedMode ??= mode
    if (!shutdownPromise) {
      shutdownPromise = (async () => {
        await socket.stop().catch((error) => console.error(error))
        await postmaster.shutdown(requestedMode).catch((error) =>
          console.error(error),
        )
        return reason
      })()
    }
    return shutdownPromise
  }

  process.on('SIGTERM', () => void shutdown('smart', 'SIGTERM'))
  process.on('SIGINT', () => void shutdown('fast', 'SIGINT'))
  process.on('SIGQUIT', () => void shutdown('immediate', 'SIGQUIT'))
  process.on('SIGHUP', () => {
    try {
      postmaster.reload()
    } catch (error) {
      console.error(error)
    }
  })

  let status = 'pass'
  let reason = 'postmaster-exit'
  let postmasterExit
  try {
    const address = await socket.start()
    await writeLifecycle(pgdata, {
      schema: PROVIDER_SCHEMA,
      status: 'ready',
      providerRevision: config.postgresRevision,
      pid: process.pid,
      pgdata,
      address,
      startedAt: new Date(startedAt).toISOString(),
      serverArgs: args,
      diagnostics: postmaster.diagnostics(),
    })
    postmasterExit = await postmaster.waitForExit()
    if (!requestedMode) {
      status = 'fail'
      reason = 'unexpected-postmaster-exit'
      await shutdown('immediate', reason)
    } else {
      reason = await shutdownPromise
    }
  } catch (error) {
    status = 'fail'
    reason = 'provider-error'
    console.error(error)
    await shutdown('immediate', reason)
  } finally {
    clearInterval(sampler)
    await shutdownPromise?.catch(() => undefined)
    peak = maximumSample(peak, sample(postmaster))
    const result = {
      schema: PROVIDER_SCHEMA,
      status,
      reason,
      pid: process.pid,
      pgdata,
      elapsedMs: Date.now() - startedAt,
      postmasterExit,
      shutdown: postmaster.diagnostics(),
      peak,
    }
    await writeClusterResult(result)
    await rm(join(pgdata, STATE_FILE), { force: true })
  }
  if (status !== 'pass') process.exitCode = 1
}

async function runPgCtl(args) {
  if (printVersionOrHelp('pg_ctl', args)) return
  const parsed = parsePgCtl(args)
  const pgdata = canonicalDataDirectory(parsed.pgdata)
  if (parsed.action === 'start') {
    await startServer(pgdata, parsed)
    return
  }
  if (parsed.action === 'restart') {
    const previous = await readLifecycle(pgdata, false)
    const serverArgs = previous?.serverArgs ?? []
    const log = parsed.log ?? previous?.log
    await stopServer(pgdata, parsed.mode, parsed.timeout, false)
    await startServer(pgdata, { ...parsed, log, options: serverArgs })
    return
  }
  if (parsed.action === 'stop') {
    await stopServer(pgdata, parsed.mode, parsed.timeout, parsed.silent)
    return
  }
  if (parsed.action === 'status') {
    const state = await readLifecycle(pgdata, false)
    if (state && isProcessAlive(state.pid)) {
      console.log(`pg_ctl: server is running (PID: ${state.pid})`)
      return
    }
    console.log('pg_ctl: no server running')
    process.exitCode = 3
    return
  }
  if (parsed.action === 'reload') {
    const state = await requireLiveLifecycle(pgdata)
    process.kill(state.pid, 'SIGHUP')
    return
  }
  fail(`pg_ctl: action is not supported by PGlite provider: ${parsed.action}`)
}

async function startServer(pgdata, options) {
  await removeStaleLifecycle(pgdata)
  const current = await readLifecycle(pgdata, false)
  if (current && isProcessAlive(current.pid)) {
    fail(`pg_ctl: another server is already running for ${pgdata}`)
  }
  const serverArgs = Array.isArray(options.options)
    ? options.options
    : splitShellWords(options.options ?? '')
  const args = ['-D', pgdata, ...serverArgs]
  const logPath = options.log ? resolve(options.log) : undefined
  let output = 'inherit'
  let logDescriptor
  if (logPath) {
    await mkdir(dirname(logPath), { recursive: true })
    logDescriptor = openSync(logPath, 'a')
    output = logDescriptor
  }
  const child = spawn(join(providerRoot, 'bin', 'postgres'), args, {
    env: { ...process.env, PGLITE_TEST_PROVIDER: providerRoot },
    detached: false,
    stdio: ['ignore', output, output],
  })
  child.unref()
  if (logDescriptor !== undefined) closeSync(logDescriptor)
  const deadline = Date.now() + options.timeout * 1_000
  while (Date.now() < deadline) {
    const state = await readLifecycle(pgdata, false)
    if (state?.status === 'ready' && state.pid === child.pid) {
      if (logPath) {
        state.log = logPath
        await writeLifecycle(pgdata, state)
      }
      return
    }
    if (!isProcessAlive(child.pid)) {
      fail(`pg_ctl: server exited before becoming ready; see ${logPath ?? 'stderr'}`)
    }
    await delay(100)
  }
  fail(`pg_ctl: server did not become ready within ${options.timeout} seconds`)
}

async function stopServer(pgdata, mode, timeout, silent) {
  const state = await readLifecycle(pgdata, false)
  if (!state || !isProcessAlive(state.pid)) {
    if (!silent) console.error('pg_ctl: PID file does not exist or server is not running')
    process.exitCode = 1
    return
  }
  const signal =
    mode === 'smart' ? 'SIGTERM' : mode === 'fast' ? 'SIGINT' : 'SIGQUIT'
  process.kill(state.pid, signal)
  const deadline = Date.now() + timeout * 1_000
  while (Date.now() < deadline) {
    const lifecycle = await readLifecycle(pgdata, false)
    if (!isProcessAlive(state.pid) && !lifecycle) return
    await delay(100)
  }
  fail(`pg_ctl: server did not shut down within ${timeout} seconds`)
}

function parseInitdb(args) {
  let pgdata
  const initdbArgs = []
  const valueOptions = new Set([
    '-E', '--encoding', '--locale', '--locale-provider', '--icu-locale',
    '--icu-rules', '--lc-collate', '--lc-ctype', '--lc-messages',
    '--lc-monetary', '--lc-numeric', '--lc-time', '-U', '--username',
    '-A', '--auth', '--auth-local', '--auth-host',
  ])
  const flagOptions = new Set([
    '--allow-group-access', '--data-checksums', '--no-data-checksums',
    '--debug', '--no-clean', '--no-instructions', '--no-locale', '--no-sync',
  ])
  for (let i = 0; i < args.length; i++) {
    const arg = args[i]
    if (arg === '-D' || arg === '--pgdata') {
      pgdata = requiredValue(args, ++i, arg)
    } else if (arg.startsWith('--pgdata=')) {
      pgdata = arg.slice('--pgdata='.length)
    } else if (valueOptions.has(arg)) {
      initdbArgs.push(arg, requiredValue(args, ++i, arg))
    } else if ([...valueOptions].some((name) =>
      name.startsWith('--') && arg.startsWith(`${name}=`),
    )) {
      initdbArgs.push(arg)
    } else if (flagOptions.has(arg)) {
      initdbArgs.push(arg)
    } else if (arg === '--waldir' || arg.startsWith('--waldir=') ||
               arg === '--pwfile' || arg.startsWith('--pwfile=')) {
      fail(`initdb: option requires an unimplemented host-path capability: ${arg}`)
    } else if (!arg.startsWith('-') && pgdata === undefined) {
      pgdata = arg
    } else {
      fail(`initdb: unsupported option: ${arg}`)
    }
  }
  if (!pgdata) fail('initdb: data directory must be specified with -D/--pgdata')
  return { pgdata, initdbArgs }
}

function parsePostgres(args) {
  let pgdata
  const startParams = []
  const settings = new Map()
  for (let i = 0; i < args.length; i++) {
    const arg = args[i]
    if (arg === '-D' || arg === '--data-directory') {
      pgdata = requiredValue(args, ++i, arg)
    } else if (arg.startsWith('--data-directory=')) {
      pgdata = arg.slice('--data-directory='.length)
    } else if (arg === '-c') {
      const value = requiredValue(args, ++i, arg)
      startParams.push('-c', value)
      recordSetting(settings, value)
    } else if (arg === '-k' || arg === '-p' || arg === '-h') {
      const value = requiredValue(args, ++i, arg)
      startParams.push(arg, value)
      settings.set(
        arg === '-k' ? 'unix_socket_directories' :
          arg === '-p' ? 'port' : 'listen_addresses',
        value,
      )
    } else if (arg === '-F') {
      startParams.push(arg)
    } else if (arg === '-d') {
      startParams.push(arg, requiredValue(args, ++i, arg))
    } else if (arg.startsWith('--') && arg.includes('=')) {
      startParams.push(arg)
      recordSetting(settings, arg.slice(2))
    } else {
      fail(`postgres: unsupported provider option: ${arg}`)
    }
  }
  if (!pgdata) fail('postgres: data directory must be specified with -D')
  return { pgdata, startParams, settings }
}

function parsePgCtl(args) {
  let pgdata
  let action
  let mode = 'fast'
  let timeout = 60
  let log
  let options = ''
  let silent = false
  for (let i = 0; i < args.length; i++) {
    const arg = args[i]
    if (arg === '-D' || arg === '--pgdata') {
      pgdata = requiredValue(args, ++i, arg)
    } else if (arg.startsWith('--pgdata=')) {
      pgdata = arg.slice('--pgdata='.length)
    } else if (arg === '-m' || arg === '--mode') {
      mode = requiredValue(args, ++i, arg)
    } else if (arg.startsWith('--mode=')) {
      mode = arg.slice('--mode='.length)
    } else if (arg === '-t' || arg === '--timeout') {
      timeout = Number.parseInt(requiredValue(args, ++i, arg), 10)
    } else if (arg.startsWith('--timeout=')) {
      timeout = Number.parseInt(arg.slice('--timeout='.length), 10)
    } else if (arg === '-l' || arg === '--log') {
      log = requiredValue(args, ++i, arg)
    } else if (arg.startsWith('--log=')) {
      log = arg.slice('--log='.length)
    } else if (arg === '-o' || arg === '--options') {
      options = requiredValue(args, ++i, arg)
    } else if (arg.startsWith('--options=')) {
      options = arg.slice('--options='.length)
    } else if (arg === '-s' || arg === '--silent') {
      silent = true
    } else if (arg === '-w' || arg === '--wait' || arg === '-W' || arg === '--no-wait') {
      // The provider always waits: its lifecycle state is the synchronization contract.
    } else if (!arg.startsWith('-') && action === undefined) {
      action = arg
    } else {
      fail(`pg_ctl: unsupported provider option: ${arg}`)
    }
  }
  if (!pgdata) fail('pg_ctl: data directory must be specified with -D/--pgdata')
  if (!action) fail('pg_ctl: action is required')
  if (!['smart', 'fast', 'immediate'].includes(mode)) {
    fail(`pg_ctl: unsupported shutdown mode: ${mode}`)
  }
  if (!Number.isInteger(timeout) || timeout <= 0) {
    fail(`pg_ctl: invalid timeout: ${timeout}`)
  }
  return { pgdata, action, mode, timeout, log, options, silent }
}

function readPostgresqlSettings(path) {
  const settings = new Map()
  if (!existsSync(path)) return settings
  for (const line of readFileSync(path, 'utf8').split(/\r?\n/)) {
    const withoutComment = stripConfigComment(line).trim()
    const match = /^([A-Za-z_][A-Za-z0-9_.]*)\s*=\s*(.*?)\s*$/.exec(withoutComment)
    if (!match) continue
    settings.set(match[1].toLowerCase(), unquoteSetting(match[2]))
  }
  return settings
}

function stripConfigComment(line) {
  let quote = false
  for (let index = 0; index < line.length; index++) {
    if (line[index] === "'" && line[index - 1] !== '\\') quote = !quote
    if (line[index] === '#' && !quote) return line.slice(0, index)
  }
  return line
}

function unquoteSetting(value) {
  const trimmed = value.trim()
  if (trimmed.startsWith("'") && trimmed.endsWith("'")) {
    return trimmed.slice(1, -1).replaceAll("''", "'")
  }
  return trimmed
}

function splitSettingList(value) {
  return unquoteSetting(String(value)).split(',').map((item) => item.trim()).filter(Boolean)
}

function recordSetting(settings, assignment) {
  const separator = assignment.indexOf('=')
  if (separator <= 0) fail(`postgres: invalid setting: ${assignment}`)
  settings.set(
    assignment.slice(0, separator).trim().toLowerCase().replaceAll('-', '_'),
    unquoteSetting(assignment.slice(separator + 1)),
  )
}

function splitShellWords(text) {
  const result = []
  let value = ''
  let quote
  let escaped = false
  for (const char of text) {
    if (escaped) {
      value += char
      escaped = false
    } else if (char === '\\' && quote !== "'") {
      escaped = true
    } else if ((char === "'" || char === '"') && (!quote || quote === char)) {
      quote = quote ? undefined : char
    } else if (/\s/.test(char) && !quote) {
      if (value) result.push(value)
      value = ''
    } else {
      value += char
    }
  }
  if (quote || escaped) fail(`pg_ctl: malformed --options value: ${text}`)
  if (value) result.push(value)
  return result
}

async function removeStaleLifecycle(pgdata) {
  const state = await readLifecycle(pgdata, false)
  if (state && isProcessAlive(state.pid)) {
    fail(`provider: live server ${state.pid} already owns ${pgdata}`)
  }
  if (state) await rm(join(pgdata, STATE_FILE), { force: true })
  const pidPath = join(pgdata, 'postmaster.pid')
  if (existsSync(pidPath)) {
    const pid = Number.parseInt(readFileSync(pidPath, 'utf8').split(/\r?\n/, 1)[0], 10)
    if (Number.isInteger(pid) && isProcessAlive(pid)) {
      fail(`provider: live postmaster PID ${pid} already owns ${pgdata}`)
    }
    await rm(pidPath, { force: true })
  }
}

async function requireLiveLifecycle(pgdata) {
  const state = await readLifecycle(pgdata, true)
  if (!isProcessAlive(state.pid)) fail(`provider: server PID ${state.pid} is not running`)
  return state
}

async function readLifecycle(pgdata, required) {
  const path = join(pgdata, STATE_FILE)
  try {
    const state = JSON.parse(await readFile(path, 'utf8'))
    if (state.schema !== PROVIDER_SCHEMA || state.pgdata !== pgdata ||
        !Number.isInteger(state.pid)) {
      fail(`provider: invalid lifecycle state: ${path}`)
    }
    return state
  } catch (error) {
    if (!required && error?.code === 'ENOENT') return undefined
    throw error
  }
}

async function writeLifecycle(pgdata, state) {
  const path = join(pgdata, STATE_FILE)
  const temporary = `${path}.${process.pid}.tmp`
  await writeFile(temporary, `${JSON.stringify(state, null, 2)}\n`, { mode: 0o600 })
  await rename(temporary, path)
}

async function writeClusterResult(result) {
  const directory = join(config.resultsRoot, 'clusters')
  await mkdir(directory, { recursive: true })
  const name = `${basename(result.pgdata).replaceAll(/[^A-Za-z0-9_.-]/g, '_')}-${result.pid}.json`
  await writeFile(join(directory, name), `${JSON.stringify(result, null, 2)}\n`)
}

function canonicalDataDirectory(path) {
  if (!path) fail('provider: PGDATA is required')
  const absolute = resolve(path)
  const parent = realpathSync(dirname(absolute))
  return join(parent, basename(absolute))
}

function isProcessAlive(pid) {
  if (!Number.isInteger(pid) || pid <= 0) return false
  try {
    process.kill(pid, 0)
    return true
  } catch (error) {
    return error?.code === 'EPERM'
  }
}

function printVersionOrHelp(program, args) {
  if (args.length === 1 && ['--version', '-V'].includes(args[0])) {
    console.log(`${program} (PostgreSQL) ${VERSION}`)
    return true
  }
  if (args.length === 1 && ['--help', '-?'].includes(args[0])) {
    console.log(`${program} (PostgreSQL) ${VERSION} PGlite test-provider adapter`)
    return true
  }
  return false
}

function parsePort(value) {
  const port = Number.parseInt(value, 10)
  if (!Number.isInteger(port) || port <= 0 || port > 65_535) {
    fail(`provider: invalid PostgreSQL port: ${value}`)
  }
  return port
}

function requiredValue(args, index, option) {
  if (index >= args.length) fail(`${option} requires a value`)
  return args[index]
}

function validateConfig(value) {
  assert.equal(value.schema, PROVIDER_SCHEMA, 'unsupported provider config schema')
  assert.equal(value.architecture, process.arch, 'provider architecture mismatch')
  for (const path of [
    value.repoRoot,
    value.icuArchive,
    value.workerFilesystemModule,
    value.artifact?.wasm,
    value.artifact?.glue,
    value.artifact?.data,
    value.resultsRoot,
  ]) {
    assert.equal(typeof path, 'string', 'provider config path is missing')
  }
}

function sample(postmaster) {
  const memory = process.memoryUsage()
  const diagnostics = postmaster.diagnostics()
  return {
    rss: memory.rss,
    heapTotal: memory.heapTotal,
    heapUsed: memory.heapUsed,
    external: memory.external,
    arrayBuffers: memory.arrayBuffers,
    liveProcesses: diagnostics.liveProcesses,
    livePrivateMemories: diagnostics.livePrivateMemories,
    privateMemoryBytes: diagnostics.privateMemoryBytes,
    globalMemoryBytes: diagnostics.globalMemoryBytes,
  }
}

function maximumSample(left, right) {
  return Object.fromEntries(
    Object.keys(left).map((key) => [key, Math.max(left[key], right[key])]),
  )
}

function delay(milliseconds) {
  return new Promise((resolveDelay) => setTimeout(resolveDelay, milliseconds))
}

function fail(message) {
  throw new Error(message)
}
