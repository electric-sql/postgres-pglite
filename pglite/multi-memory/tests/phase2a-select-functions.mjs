import { readFile } from 'node:fs/promises'

const [cpuProfilePath, entryProfilePath, selection = 'cpu:512'] =
  process.argv.slice(2)
if (!entryProfilePath) {
  throw new Error(
    'usage: phase2a-select-functions.mjs CPU_PROFILE ENTRY_PROFILE cpu:N|executed',
  )
}

const [cpuProfile, entryProfile] = await Promise.all(
  [cpuProfilePath, entryProfilePath].map((path) =>
    readFile(path, 'utf8').then(JSON.parse),
  ),
)

let indices
if (selection === 'executed') {
  indices = entryProfile.functions.map((entry) => entry.wasmFunctionIndex)
} else {
  const match = /^cpu:(\d+)$/.exec(selection)
  if (!match || Number(match[1]) < 1) {
    throw new Error(`invalid selection ${selection}`)
  }
  indices = cpuProfile.functions
    .slice(0, Number(match[1]))
    .map((entry) => entry.wasmFunctionIndex)
}

for (const index of [...new Set(indices)]) {
  console.log(index)
}
