#!/usr/bin/env node

import assert from 'node:assert/strict'
import { copyFileSync, mkdirSync, rmSync, writeFileSync } from 'node:fs'
import { basename, join, resolve } from 'node:path'
import { spawnSync } from 'node:child_process'

const options = parseArguments(process.argv.slice(2))
rmSync(options.outputDirectory, { recursive: true, force: true })
mkdirSync(options.outputDirectory, { recursive: true })

for (const module of options.modules) {
  module.symbols = definedFunctions(options.llvmNm, module.objects)
}

const owners = new Map()
for (const module of options.modules) {
  for (const symbol of module.symbols) {
    const modules = owners.get(symbol) ?? new Set()
    modules.add(module.name)
    owners.set(symbol, modules)
  }
}
const duplicated = new Set(
  [...owners].filter(([, modules]) => modules.size > 1).map(([name]) => name),
)
assert.deepEqual(
  [...duplicated],
  [],
  `static module symbols must be namespaced before generation: ${[
    ...duplicated,
  ].join(', ')}`,
)

const copiedObjects = []
for (const module of options.modules) {
  module.copiedObjects = module.objects.map((object, index) => {
    const output = join(
      options.outputDirectory,
      `${module.name}-${index}-${basename(object)}`,
    )
    copyFileSync(object, output)
    copiedObjects.push(output)
    return output
  })
  const linkedSymbols = definedFunctions(options.llvmNm, module.copiedObjects)
  const magic = `pgl_static_${module.name}_Pg_magic_func`
  assert.ok(linkedSymbols.has(magic), `${module.name} has no namespaced magic`)
  const requestedByResolved = new Map(
    [...module.aliases].map(([requested, resolved]) => [resolved, requested]),
  )
  for (const [requested, resolved] of module.aliases) {
    assert.ok(
      linkedSymbols.has(resolved),
      `${module.name} alias ${requested} has no resolved symbol ${resolved}`,
    )
  }
  module.resolvedSymbols = new Map(
    [...module.symbols].map((resolved) => [
      requestedByResolved.get(resolved) ?? resolved,
      resolved,
    ]),
  )
  assert.equal(
    module.resolvedSymbols.size,
    module.symbols.size,
    `${module.name} aliases must not collide with another requested symbol`,
  )
  module.resolvedSymbols.set('Pg_magic_func', magic)
}

const resolvedOwners = new Map()
for (const module of options.modules) {
  for (const resolved of module.resolvedSymbols.values()) {
    const owner = resolvedOwners.get(resolved)
    if (owner === undefined) resolvedOwners.set(resolved, module.name)
    else {
      assert.equal(
        owner,
        module.name,
        `static symbol ${resolved} is still duplicated by ${owner} and ${module.name}`,
      )
    }
  }
}

writeFileSync(
  join(options.outputDirectory, 'static-test-modules.c'),
  generateRegistry(options.modules),
)
writeFileSync(
  join(options.outputDirectory, 'objects.rsp'),
  `${copiedObjects.join('\n')}\n`,
)
writeFileSync(
  join(options.outputDirectory, 'manifest.json'),
  `${JSON.stringify(
    {
      schema: 1,
      modules: options.modules.map((module) => ({
        name: module.name,
        objects: module.objects.map((object) => resolve(object)),
        symbols: module.symbols.size,
        magic: module.resolvedSymbols.get('Pg_magic_func'),
      })),
    },
    null,
    2,
  )}\n`,
)

function parseArguments(args) {
  let outputDirectory
  let llvmNm = '/emsdk/upstream/bin/llvm-nm'
  const modules = []
  for (let index = 0; index < args.length; ) {
    const option = args[index++]
    if (option === '--output-dir') outputDirectory = args[index++]
    else if (option === '--llvm-nm') llvmNm = args[index++]
    else if (option === '--module') {
      const name = args[index++]
      const objects = []
      while (index < args.length && !args[index].startsWith('--')) {
        objects.push(args[index++])
      }
      assert.match(name, /^[a-z][a-z0-9_]*$/)
      assert.ok(objects.length > 0, `${name} has no objects`)
      modules.push({ name, objects, aliases: new Map() })
    } else if (option === '--alias') {
      const value = args[index++]
      const match = value.match(
        /^([a-z][a-z0-9_]*):([A-Za-z_$][A-Za-z0-9_$]*)=([A-Za-z_$][A-Za-z0-9_$]*)$/,
      )
      assert.ok(match, `invalid static-module alias: ${value}`)
      const module = modules.find(({ name }) => name === match[1])
      assert.ok(module, `alias names unknown module ${match[1]}`)
      assert.ok(
        !module.aliases.has(match[2]),
        `${module.name} repeats alias ${match[2]}`,
      )
      module.aliases.set(match[2], match[3])
    } else throw new Error(`unknown option: ${option}`)
  }
  assert.ok(outputDirectory, '--output-dir is required')
  assert.ok(modules.length > 0, 'at least one --module is required')
  assert.equal(new Set(modules.map(({ name }) => name)).size, modules.length)
  return { outputDirectory: resolve(outputDirectory), llvmNm, modules }
}

function definedFunctions(llvmNm, objects) {
  const symbols = new Set()
  for (const object of objects) {
    const output = run(llvmNm, ['--defined-only', '--extern-only', object])
    for (const line of output.split('\n')) {
      const match = line.trim().match(/^[0-9a-fA-F]+\s+([Tt])\s+(\S+)$/)
      if (!match) continue
      assert.match(match[2], /^[A-Za-z_$][A-Za-z0-9_$]*$/)
      symbols.add(match[2])
    }
  }
  return symbols
}

function run(command, args) {
  const result = spawnSync(command, args, { encoding: 'utf8' })
  if (result.status !== 0) {
    throw new Error(
      `${command} ${args.join(' ')} failed (${result.status}):\n${result.stderr}`,
    )
  }
  return result.stdout
}

function generateRegistry(modules) {
  const declarations = new Set()
  for (const module of modules) {
    for (const symbol of module.resolvedSymbols.values()) {
      declarations.add(`extern void ${symbol}(void);`)
    }
  }
  const moduleTables = modules
    .map((module) => {
      const entries = [...module.resolvedSymbols]
        .filter(([symbol]) => symbol !== 'Pg_magic_func')
        .sort(([left], [right]) => left.localeCompare(right))
        .map(
          ([requested, resolved]) =>
            `\t{"${cString(requested)}", ${resolved}},`,
        )
        .join('\n')
      return `static const struct pgl_static_symbol ${module.name}_symbols[] = {
${entries}
};`
    })
    .join('\n\n')
  const handles = modules
    .map((module) => {
      const magic = module.resolvedSymbols.get('Pg_magic_func')
      return `\t{"${module.name}.so", ${magic}, ${module.name}_symbols,
\t sizeof(${module.name}_symbols) / sizeof(${module.name}_symbols[0])},`
    })
    .join('\n')
  return `/* Generated test artifact input; do not edit. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PGL_STATIC_DL_NOT_HANDLED ((void *) (intptr_t) -1)

typedef void (*pgl_static_function)(void);

struct pgl_static_symbol
{
\tconst char *name;
\tpgl_static_function address;
};

struct pgl_static_module
{
\tconst char *filename;
\tpgl_static_function magic;
\tconst struct pgl_static_symbol *symbols;
\tsize_t symbol_count;
};

${[...declarations].sort().join('\n')}

${moduleTables}

static const struct pgl_static_module modules[] = {
${handles}
};

void *
pgl_static_dlopen(const char *filename, int flags)
{
\tconst char *base;
\tsize_t index;

\t(void) flags;
\tbase = strrchr(filename, '/');
\tbase = base == NULL ? filename : base + 1;
\tfor (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++)
\t{
\t\tif (strcmp(base, modules[index].filename) == 0)
\t\t\treturn (void *) &modules[index];
\t}
\treturn PGL_STATIC_DL_NOT_HANDLED;
}

void *
pgl_static_dlsym(void *handle, const char *name)
{
\tsize_t module_index;

\tfor (module_index = 0;
\t\t module_index < sizeof(modules) / sizeof(modules[0]);
\t\t module_index++)
\t{
\t\tconst struct pgl_static_module *module = &modules[module_index];
\t\tsize_t symbol_index;

\t\tif (handle != (void *) module)
\t\t\tcontinue;
\t\tif (strcmp(name, "Pg_magic_func") == 0)
\t\t\treturn (void *) module->magic;
\t\tfor (symbol_index = 0; symbol_index < module->symbol_count;
\t\t\t symbol_index++)
\t\t{
\t\t\tif (strcmp(name, module->symbols[symbol_index].name) == 0)
\t\t\t\treturn (void *) module->symbols[symbol_index].address;
\t\t}
\t\treturn NULL;
\t}
\treturn PGL_STATIC_DL_NOT_HANDLED;
}

int
pgl_static_dlclose(void *handle)
{
\tsize_t index;

\tfor (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++)
\t{
\t\tif (handle == (void *) &modules[index])
\t\t\treturn 0;
\t}
\treturn -2;
}
`
}

function cString(value) {
  return value.replaceAll('\\', '\\\\').replaceAll('"', '\\"')
}
