#!/usr/bin/env node
import { basename, dirname, join } from "path";

const invokedPath = process.argv[1];
const invokedName = basename(invokedPath).replace(/\.mjs$/, "");

if (invokedName.startsWith("run_")) {
  console.error("This script should be invoked via a symlink (e.g., pg_config, pg_ctl)");
  process.exit(1);
}

const moduleName = invokedName + ".mjs";
const modulePath = join(dirname(invokedPath), moduleName);

const { default: Module } = await import(modulePath);

if (typeof Module.main === "function") {
  await Module.main();
} else if (typeof Module === "function") {
  await Module();
} else {
  console.error("⚠️ Module has no callable entry point");
}
