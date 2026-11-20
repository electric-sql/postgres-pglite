import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { test } from 'node:test'
import { runInNewContext } from 'node:vm'

const source = readFileSync(
  new URL('../../src/pglitec/directory-entries.js', import.meta.url),
  'utf8',
)

test('normalizes dot entries for every filesystem while retaining startup hooks', () => {
  for (const entries of [
    [],
    ['file'],
    ['.', '..', 'file'],
    ['..', '.', '.', 'file'],
  ]) {
    let initialized = false
    const FS = {
      readdir(path) {
        assert.equal(this, FS)
        assert.equal(path, '/data')
        return entries
      },
    }
    const Module = {
      preRun() {
        initialized = true
      },
    }
    runInNewContext(source, { FS, Module })
    for (const hook of Module.preRun) hook()
    assert.equal(initialized, true)
    assert.deepEqual(Array.from(FS.readdir('/data')), [
      '.',
      '..',
      ...entries.filter((entry) => entry !== '.' && entry !== '..'),
    ])
    assert.deepEqual(Array.from(FS.readdir('/data')), [
      '.',
      '..',
      ...entries.filter((entry) => entry !== '.' && entry !== '..'),
    ])
  }
})

test('retains filesystem errors and an existing startup-hook array', () => {
  const failure = new Error('directory unavailable')
  const FS = {
    readdir() {
      throw failure
    },
  }
  let initialized = 0
  const Module = { preRun: [() => initialized++] }
  runInNewContext(source, { FS, Module })
  for (const hook of Module.preRun) hook()
  assert.equal(initialized, 1)
  assert.throws(
    () => FS.readdir('/missing'),
    (error) => error === failure,
  )
})
