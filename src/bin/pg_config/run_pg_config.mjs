#!/usr/bin/env node
import Module from "./pg_config.mjs";

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
  const exeInfo = FS.analyzePath(fullPath);
  if (!exeInfo.exists) {
    const exePathArray = fullPath.split('/');
    const exeName = exePathArray.pop();
    const dirPath = exePathArray.join('/');
    const pathInfo = FS.analyzePath(dirPath);
    if (!pathInfo.exists) {
      FS.mkdirTree(dirPath);
    }
    const view = new Int32Array(new ArrayBuffer(0));
    FS.createDataFile(dirPath, exeName, view, true, false, true);
    FS.chmod(fullPath, 0o0755);
  }
}