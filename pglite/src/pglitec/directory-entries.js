// Emscripten's directory syscall consumes FS.readdir for every mounted VFS.
var pgliteDirectoryHooks = Module['preRun'] || []
if (typeof pgliteDirectoryHooks === 'function')
  pgliteDirectoryHooks = [pgliteDirectoryHooks]
pgliteDirectoryHooks.unshift(function () {
  var readdir = FS.readdir
  FS.readdir = function () {
    var entries = readdir.apply(this, arguments)
    return ['.', '..'].concat(
      entries.filter(function (entry) {
        return entry !== '.' && entry !== '..'
      }),
    )
  }
})
Module['preRun'] = pgliteDirectoryHooks
