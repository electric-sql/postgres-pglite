# PGlite multi-memory Phase 0

This directory contains the correctness-first WebAssembly multi-memory
transformer and its Phase 0 verification corpus.

All compiler and test tooling runs in a Docker image derived from the pinned
PGlite Wasm builder. The image builds the transformer against Binaryen commit
`52bc45fc34ec6868400216074744147e9d922685`, the revision shipped with
Emscripten 3.1.74.

Run the complete milestone from the `postgres-pglite` checkout:

```sh
./pglite/multi-memory/run-phase0.sh
```

The runner:

1. generates an exhaustive single-memory fixture;
2. assembles it with the pinned Binaryen parser;
3. transforms it into the two-domain tagged-pointer ABI;
4. validates the output and the fail-closed opcode report;
5. runs differential, trap, alias, atomic, bulk-memory, and fuzz tests on
   Node 22;
6. checks Node 20 rejection and Node 22 runtime capabilities, including
   `Atomics.waitAsync()`, then repeats the supported-runtime gate on Node 24;
7. verifies names, source maps, ABI metadata, and deterministic output.

Generated files are written under `pglite/multi-memory/.out/` and are ignored.

The native `pglite-wasm-multi-memory` tool accepts a conventional wasm32
module with one imported memory. It preserves that private memory as index 0,
adds `pglite.global_memory` as index 1, and rewrites every dereference through
a shape-deduplicated helper. Pointer tag `00` selects the private 2 GiB
aperture, tag `10` selects the global 1 GiB aperture, null traps, and tag `11`
is reserved and traps. `memory.init`, `memory.size`, `memory.grow`, data drops,
and atomic fences are explicit private-memory allowlists.

The output carries a `pglite.multi-memory.abi` JSON custom section and the
optional report records the input hash, tool/Binaryen versions, ABI limits,
every rewritten operation count, every allowlisted operation count, and the
generated helper inventory. The sidecar build manifest adds the final output
and report hashes (the complete artifact cannot contain its own SHA-256).
Inputs with an unexpected memory topology,
memory64, an incompatible aperture, a second transformation stamp, or an
unaccounted memory operation fail closed.

The tool image is a derived version of `electricsql/pglite-builder:3.1.74-7`.
It contains the pinned Binaryen sources and native tools, Emscripten's Node 20,
and Node 22.13/24.15 for the supported-runtime gates; no host compiler,
Binaryen, WABT, or Node installation is used by the Phase 0 runner.
