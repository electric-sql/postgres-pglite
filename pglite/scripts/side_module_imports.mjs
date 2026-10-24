#!/usr/bin/env node
// Prints the symbols a linked SIDE_MODULE .so needs from the main module, one per line.
// Used for extensions that are not built with PGXS (see build-pgrouting.sh), where the
// list cannot be computed from the .o files: the .so also contains statically linked libraries.
import { readFileSync } from "fs";

// provided by the dynamic linker itself, not exported by the main module
const LINKER_SYMBOLS = new Set(["memory", "__indirect_function_table", "__stack_pointer", "__memory_base", "__table_base"]);

const module = new WebAssembly.Module(readFileSync(process.argv[2]));
const defined = new Set(WebAssembly.Module.exports(module).map((e) => e.name));
const imports = new Set();
for (const { name } of WebAssembly.Module.imports(module)) {
  // the module's own symbols can be imported too: weak definitions (ie C++ template instantiations)
  // so they can be interposed, and GOT.func / GOT.mem entries for the ones whose address is taken
  if (defined.has(name)) continue;
  if (LINKER_SYMBOLS.has(name)) continue;
  imports.add(name);
}
console.log([...imports].sort().join("\n"));
