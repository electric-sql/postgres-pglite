# Reviewed compilation adapters

The build uses `policy.json` to redirect selected calls before compiling C.
Postmaster rules select memory placement, loader metadata lookup, backend
signal registration, and executable discovery. Classic builds skip those rules.
Three reviewed files undefine `__PGLITE__` to restore upstream postmaster
checkpoint and GUC behavior; `__PGLITE_POSTMASTER__` remains defined. Both modes apply static
frontend encoding flags in the compiler driver. PostgreSQL source stays unchanged
at those call sites; unknown allocation sites retain global placement.

Each rule records a source path, its SHA-256, the function APIs to redirect,
and the ownership reason. Review that reason before updating a source hash:
root placement is valid only when every attaching process belongs to the same
backend group. The compiler does not infer that contract from arbitrary DSM
handles or from allocations occurring inside a query.

`dsm_create` redirects to the domain-aware DSM entry point in PGlite's runtime.
Its declaration lives in the injected PGlite header. The same entry point is
linked into classic builds, where allocation remains global.
`dsa_create_ext` redirects to a PGlite runtime wrapper that selects memory 2
for construction and restores placement on success or PostgreSQL ERROR.
The wrapper lives in the main module so its error handler and longjmp target
share a Wasm module. Subsequent DSA growth follows the control segment's domain.
No transaction or query lifetime hierarchy is involved.

The build checks all reviewed source hashes and includes the policy, driver,
header, runtime helper, semaphore implementation, directory hook and supplemental
make rules in its build-profile fingerprint. A small generated
shell wrapper bypasses Node for unrelated compilation and link commands.
Successful selected compilations produce JSON records under the installed
`memory-placement` directory. Classic builds use the compiler driver for frontend
flags while retaining global allocation placement.

For a separately built extension, provide a policy and its source root through
`PGLITE_MEMORY_PLACEMENT_POLICY` and `PGLITE_MEMORY_PLACEMENT_ROOT`, and invoke
`cc.mjs` with normal postmaster Emscripten arguments. Use the matching PGlite
headers and postmaster runtime, including `pgl_root_dsa_create_ext`.
Compile extensions with `-iquote PGLITE_SOURCE/pglite/src/postgres-headers`
alongside the forced `pglitec.h`. The forwarding `miscadmin.h` retains the
upstream header and adds Worker signal dispatch at interrupt safe points.

The postmaster build selects `pglite/src/pglitec/semaphore.c` through PostgreSQL's
generated `pg_sema.c` link. `pglite/build-rules.mk` supplies backend runtime link
dependencies and export generation through `MAKEFILES`. Frontend Wasm sidecars
are staged by `build-pglite.sh`. These mechanisms leave PostgreSQL Makefiles
unchanged. `directory-entries.js` normalizes POSIX dot entries through the common
Emscripten `FS.readdir` boundary for all mounted filesystems.

Run the adapter tests in the pinned builder with:

```sh
node22 --test pglite/tests/postmaster/memory-placement.test.mjs pglite/tests/postmaster/directory-entries.test.mjs
```

The main repository's postmaster gate also compiles the unchanged `test_dsa`
module with and without placement. It checks reuse, backend-exit reclamation,
constructor and growth failures, and alternating paired timings. The policy's
`test_dsa` rule is test coverage, not evidence that arbitrary extensions are
safe to place in memory 2.
