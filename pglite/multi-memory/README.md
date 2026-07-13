# PGlite multi-memory transformer

This directory contains the correctness-first WebAssembly multi-memory
transformer and its Phase 0/1 verification corpus.

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

## Phase 1: current PGlite artifact

From the parent PGlite checkout, run:

```sh
pnpm wasm:multi-memory:phase1
```

The runner installs and invokes every compiler, Node runtime, package-test
dependency, and measurement tool inside the same derived Wasm builder image.
It builds the current package glue, transforms the release `pglite.wasm`
twice, requires byte-for-byte deterministic output, audits the embedded ABI
and complete rewrite inventory, and runs differential SQL against classic and
transformed modules. The corpus covers DDL/DML, rollback, scalar and container
types, planner/index behavior, recursive execution, upstream `pg_regress`
derived boolean/UUID cases, error metadata, and a dynamically loaded extension.
It then runs regression-recursive, indexed-aggregate, and pgbench-style
steady-state measurements in isolated Node processes, followed by the
unchanged PGlite basic and Node filesystem suites. The test runner limits
Vitest concurrency and raises only its timeout because the generic artifact's
extra compilation and startup cost otherwise creates resource-dependent test
timeouts.

Results are machine-readable under `.out/phase1/`. The runner exits nonzero
if any functional test fails or if the worst workload exceeds the design's
1.35x throughput limit. Phase 1 therefore produces a decisive no-go result,
not a silently weakened threshold: the generic-everything artifact is correct
but currently misses the performance gate. The report is the input to the
provenance/direct-access work rather than permission to proceed to postmaster
integration unchanged.

## Phase 2A: private-only performance oracle

From the parent PGlite checkout, run:

```sh
pnpm wasm:multi-memory:phase2a
```

This builds deterministic private-only-oracle, outlined-generic, and
inline-generic artifacts from the same release input. The oracle retains all
current single-user dereferences as direct memory-0 operations while adding
the complete multi-memory import and ABI surface. It is an experimental
performance ceiling and is not safe for tagged postmaster pointers.

The runner audits all oracle operations as direct-private, differentially
tests it against classic PGlite, and measures all four profiles in alternating
pairs of isolated Node processes. The oracle runs three independent
five-pair series and must satisfy the continuation bound in every series.
`.out/phase2a/summary.json` records the conservative workload result plus
artifact-size, compile, and startup ratios. The runner continues only when the
oracle is no worse than 1.15x on every agreed steady-state workload; generic
profiles remain diagnostic correctness and cost references.

After Phase 2A has produced its oracle, run the dynamic continuation from the
parent checkout:

```sh
pnpm wasm:multi-memory:phase2a-profile
```

This instruments entries to every memory-using function, takes a 100 us V8 CPU
profile, preserves optimized function names in a separate profiling link, and
matches the stripped release functions to those names using a conservative
structural fingerprint. It then sweeps diagnostic whole-function direct access
sets selected by the CPU profile. The profiling link and all analysis run in
the derived builder container. Set `PGLITE_PHASE2A_REBUILD_NAMED=true` to force
the relatively expensive named link to be rebuilt.

The 13 July 2026 profile observed 6,431,434 entries across 1,379
memory-using functions and attributed 753 of 760 samples in the main Wasm
module. Exact unique structural matches named 1,555 of the 1,725 functions
seen by either profile; ambiguous names remain explicitly ambiguous.

The diagnostic sweep found its first Phase 2 exit-threshold result with the
top 512 CPU-ranked functions direct: 1.314x worst-case throughput, with
1.197x recursive, 1.179x indexed-aggregate, and 1.314x pgbench-style ratios.
Making every workload-observed function direct reached 1.044x worst-case.
These artifacts are deliberately marked `profile-guided-private-oracle`: they
assume all pointers in selected functions are private and are therefore
unsound for tagged postmaster pointers. They prove that the 1.35x target is
reachable and quantify the amount of dispatch that must be removed; they do
not pass Phase 2 or authorize production use. The sound provenance candidate
must reproduce the improvement while retaining generic access at every
unproved site.

## Phase 2B: conservative provenance

Run the first sound specialization tranche from the parent checkout:

```sh
pnpm wasm:multi-memory:phase2b
```

The analysis uses Binaryen reaching definitions for locals, private constants,
select/phi agreement, valid constant pointer arithmetic, Emscripten's private
stack and memory-base imports, private `GOT.mem` cell addresses, validated
allocator-return summaries, and fixed-point private-parameter propagation
through functions that have only known direct callers. Exported or
table-referenced functions retain unknown parameters. The allocator manifest
is validated against the exact module's export table and deliberately excludes
ShmemAlloc, DSM, DSA, and `shm_toc` allocators.

On the current release artifact this proves 127,735 of 395,210 static memory
operations (32.3%) direct and leaves 267,475 generic. Differential SQL passes,
but the workload result is still 3.260x worst case: 2.939x recursive, 3.260x
indexed aggregate, and 1.428x pgbench-style. Static coverage therefore does not
predict useful dynamic coverage. The residual hot report shows that loop-heavy
`memcpy`, `memset`, comparison, compression, executor, and tuple paths remain
generic. The next work is sound function-boundary hoisting/private clones for
those ranked bimodal functions, not more unprofiled static-site coverage.

The Phase 2D experiment adds input-hash-pinned, function-boundary private
clones for 37 ranked functions. Each original entry checks its declared pointer
parameters once and enters the private clone only when every pointer has the
private tag; null or tagged values retain the generic body. Run it with:

```sh
pnpm wasm:multi-memory:phase2d
```

The fresh result is 3.089x worst case: 2.665x recursive, 3.089x indexed
aggregate, and 1.395x pgbench-style, at 1.028x artifact size. This is a real but
insufficient improvement. A separate sound inline-private fallback reached
1.872x worst case but inflated the artifact to 1.442x. The remaining blocker is
pointer provenance after loads from pointer-bearing structures, especially in
executor, compression, tuple, and comparison loops. The next step is targeted
root/field metadata (or an LLVM provenance pass), not more clone entries.

## Phase 2C: checked source provenance and loop specialization

Run the source-provenance build and repeated performance gate from the parent
PGlite checkout:

```sh
pnpm wasm:multi-memory:phase2c
```

The PGlite libc exposes a checked private-pointer identity and the PostgreSQL
fork uses small `__PGLITE_MULTI_MEMORY__` fences at measured executor hot
paths. Release transformation consumes and removes these identities. Debug
transformation can retain their signed-private checks. Parameter-wide facts
are accepted only when the marker assignment dominates every other read in
the Binaryen control-flow graph; a Phase 0 conditional-marker test prevents a
path-local assertion from becoming a function-wide proof.

The source annotations keep shared tuple payloads generic. They specialize
backend-private expression state, slot control and deformation arrays, and use
three compact private tuple-deformation loops selected by one payload-tag
check. Dynamically indexed cells receive their own checked identity instead
of assuming that arbitrary integer arithmetic preserves a pointer domain. The
matched classic artifact is produced from the identical source Wasm by
removing identity calls without applying the multi-memory rewrite.

The cleaned 13 July 2026 result passes the Phase 2C performance target in all
three independent series. The worst ratios were 1.303x, 1.281x, and 1.264x.
Taking the conservative maximum per workload gives 1.242x recursive, 1.303x
indexed aggregate, and 1.212x pgbench-style throughput. Differential SQL
passes 9/9 cases, both the pre-optimization and `wasm-opt -O3` outputs are
byte-for-byte deterministic, and Phase 0 remains green. The candidate has
36,664 direct loads and 91,664 direct stores; 202,486 loads and 63,412 stores
remain generic. It is 1.414x the matched classic artifact size.

This passes the bounded Phase 2C performance rescue. It does not by itself
complete Phase 2: the pointer-bearing JavaScript import/view audit and host ABI
hardening in Phase 2E remain required before the Phase 2F exit checkpoint or
postmaster work.

The native `pglite-wasm-multi-memory` tool accepts a conventional wasm32
module with one imported memory. It preserves that private memory as index 0,
adds `pglite.global_memory` as index 1 and the reserved
`pglite.scoped_memory` as index 2, and rewrites every dereference through a
shape-deduplicated helper. Pointer tag `00` selects the private 2 GiB
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
Node 22.13/24.15, and pinned pnpm 9.7.0; no host compiler, Binaryen, WABT, Node,
package manager, or test runner is used by either milestone runner.
