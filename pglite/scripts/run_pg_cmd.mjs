#!/usr/bin/env node
import { basename, dirname, join } from "path";

const invokedPath = process.argv[1];
const invokedName = basename(invokedPath);
const moduleName = invokedName + ".mjs";
const modulePath = join(dirname(invokedPath), moduleName);
const { default: Module } = await import(modulePath);

// If the module has a `main()` (or similar) method, call it:
if (typeof Module.main === "function") {
  await Module.main();
} else if (typeof Module === "function") {
  const mod = await Module();

  // postgres executables (like pg_config etc.) complain if they cannot find their respective executables
  const nodePath = process.argv.shift();
  const exePath = process.argv.shift();
  ensureExePath(mod.FS, exePath);

  mod.callMain(process.argv)
} else {
  console.error("⚠️ Module has no callable entry point");
}

function ensureExePath(FS, fullPath) {
  const exists = pathExists(FS, fullPath);
  if (!exists) {
    const exePathArray = fullPath.split('/');
    const exeName = exePathArray.pop();
    const dirPath = exePathArray.join('/');
    FS.mkdirTree(dirPath);
    const view = new Int32Array(new ArrayBuffer(0));
    FS.createDataFile(dirPath, exeName, view, true, false, true);
    FS.chmod(fullPath, 0o0755);
  }
}

function pathExists(fs, path) {
  try {
    fs.stat(path);
    return true;
  } catch (err) {
    if (err.name === 'ErrnoError' && err.code === 'ENOENT') return false;
    throw err; // Don’t hide other filesystem errors
  }
}