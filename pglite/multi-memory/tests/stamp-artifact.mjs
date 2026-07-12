#!/usr/bin/env node

import { createHash } from 'node:crypto'
import { readFile, writeFile } from 'node:fs/promises'

const [wasmPath, reportPath, outputPath] = process.argv.slice(2)
const [wasm, reportBytes] = await Promise.all([
  readFile(wasmPath),
  readFile(reportPath),
])
const report = JSON.parse(reportBytes)
const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex')
const manifest = {
  schema: 1,
  abi: report.abi,
  outputSHA256: sha256(wasm),
  reportSHA256: sha256(reportBytes),
  rewritten: report.rewritten,
  allowlisted: report.allowlisted,
}
await writeFile(outputPath, `${JSON.stringify(manifest, null, 2)}\n`)
