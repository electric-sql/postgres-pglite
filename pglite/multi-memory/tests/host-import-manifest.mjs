import { createHash } from 'node:crypto'
import { readFile, writeFile } from 'node:fs/promises'
import { pointerImportPolicy } from '../host-import-policy.mjs'

const [wasmPath, gluePath, manifestPath, mode = '--check'] =
  process.argv.slice(2)
if (!wasmPath || !gluePath || !manifestPath) {
  throw new Error(
    'usage: host-import-manifest.mjs WASM GLUE MANIFEST [--check|--write]',
  )
}

const bytes = await readFile(wasmPath)
const glue = await readFile(gluePath, 'utf8')
const module = new WebAssembly.Module(bytes)
const imports = WebAssembly.Module.imports(module)
const hash = (value) => createHash('sha256').update(value).digest('hex')

const signatures = new Map()
for (const match of glue.matchAll(/([A-Za-z_$][\w$]*)\.sig="([vijfdp]+)"/g)) {
  signatures.set(match[1], match[2])
}

const signatureFor = (name) => {
  const candidates = [
    name,
    `_${name}`,
    name.startsWith('_') ? `_${name}` : `__${name}`,
    name.startsWith('__') ? `_${name}` : undefined,
  ].filter(Boolean)
  const id = candidates.find((candidate) => signatures.has(candidate))
  return id ? signatures.get(id) : undefined
}

const seenPolicy = new Set()
const entries = imports.map((imported) => {
  const base = {
    module: imported.module,
    name: imported.name,
    kind: imported.kind,
  }
  if (imported.kind !== 'function') return base

  const key = `${imported.module}.${imported.name}`
  if (imported.name.startsWith('invoke_')) {
    return {
      ...base,
      class: 'opaque-indirect',
      signature: imported.name.slice('invoke_'.length),
      pointers: [],
      returnPointer: 'none',
      note: 'i32 values are opaque Wasm table-call arguments; JavaScript must not dereference them',
    }
  }

  const signature = signatureFor(imported.name)
  if (!signature) {
    throw new Error(`generated glue signature not found for ${key}`)
  }
  const policy = pointerImportPolicy[key]
  if (!signature.includes('p')) {
    if (policy) throw new Error(`unnecessary pointer policy for ${key}`)
    return {
      ...base,
      class: 'scalar',
      signature,
      pointers: [],
      returnPointer: 'none',
    }
  }
  if (!policy) throw new Error(`pointer-width ABI is unclassified for ${key}`)
  seenPolicy.add(key)

  const pointerWidthParameters = [...signature.slice(1)].flatMap(
    (type, index) => (type === 'p' ? [index] : []),
  )
  const classified = [
    ...policy.pointers.map(({ index }) => index),
    ...policy.pointerSizedScalars,
  ].sort((a, b) => a - b)
  if (JSON.stringify(pointerWidthParameters) !== JSON.stringify(classified)) {
    throw new Error(
      `pointer-width parameters are not classified exactly once for ${key}: ` +
        `signature=${pointerWidthParameters}, policy=${classified}`,
    )
  }
  if (
    (signature[0] === 'p') !==
    (policy.returnPointer !== 'none' || policy.pointerSizedReturn === true)
  ) {
    throw new Error(`pointer-width return is incorrectly classified for ${key}`)
  }

  return { ...base, signature, ...policy }
})

for (const key of Object.keys(pointerImportPolicy)) {
  if (!seenPolicy.has(key)) throw new Error(`stale pointer policy: ${key}`)
}

const manifest = {
  version: 1,
  emscriptenVersion: '3.1.74',
  wasmSha256: hash(bytes),
  glueSha256: hash(glue),
  entries,
}
const formatted = `${JSON.stringify(manifest, null, 2)}\n`

if (mode === '--write') {
  await writeFile(manifestPath, formatted)
} else if (mode === '--check') {
  const expected = await readFile(manifestPath, 'utf8')
  if (expected !== formatted) {
    throw new Error('host import manifest is stale; regenerate it explicitly')
  }
} else {
  throw new Error(`unknown mode: ${mode}`)
}

const functions = entries.filter(({ kind }) => kind === 'function')
const pointerBearing = functions.filter(({ pointers }) => pointers?.length)
const counts = Object.fromEntries(
  ['scalar', 'opaque-indirect', 'private-only', 'tagged'].map((kind) => [
    kind,
    functions.filter(({ class: value }) => value === kind).length,
  ]),
)
console.log(
  JSON.stringify(
    {
      imports: entries.length,
      functions: functions.length,
      pointerBearing: pointerBearing.length,
      pointerParameters: pointerBearing.reduce(
        (sum, { pointers }) => sum + pointers.length,
        0,
      ),
      counts,
    },
    null,
    2,
  ),
)
