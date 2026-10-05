# PGlite

This is an overview of the changes to the upstream PostgreSQL code needed for PGlite - Postgres in WASM. We aim to keep this changes to a minimum, to rely as much as possible on the original code to make maintenance easier.

## Convention

All our changes to the upstream PostgreSQL code are marked with a `__PGLITE__` definition, most often like this:

```
#ifdef __PGLITE__
...
#endif
```

A search for `__PGLITE__` should reveal all the points where we intervened.

In addition, we added a top-level folder called `pglite` that contains additional backend code (see `pglitec.c`) as well as git submodules for external extensions, helper scripts and static data. Additionally, see `build-with-docker.sh` and `build-pglite.sh` scripts at the top level for entry points into the build process.

## Loop unrolling

When there is no more data to process, Postgres blocks waiting for more. PGlite on the other hand, needs to be able to exit the processing loop. This is achieve by "unrolling the main loop". Specifically, in `src/backend/tcop/postgres.c` we extracted the processing logic out from the main loop to a separate function:

```
	for (;;)
	{
		PostgresMainLoopOnce(); // new function containing logic between brackets in original
	}
```

This allows us to directly call `PostgresMainLoopOnce()` whenever we receive a new query. Exiting this function is handled in such a way as to allow a real process exit (when PGlite is not active) or a fake exit (which keeps the process alive, doesn't run any `atexit`s etc.):


```
	#ifdef __PGLITE__
	if (is_pglite_active != 0)
		exit(PGLITE_EXIT_ALIVE);
	else 
		proc_exit(0);
	#else
	proc_exit(0);
	#endif
```                

We also want to be able to call `main()` and let Postgres behave as a native deployment. This is useful, for example, when running `initdb` against PGlite.

## Exception handling

PostgreSQL relies on a top-level `sigsetjmp()` to catch any exception that might occur (ie code that triggers the `siglongjmp()` to the top). We extracted the logic to handle the longjmp to a separate function, again to enable us to call it directly:

```
	if (sigsetjmp(postgresmain_sigjmp_buf, 1) != 0)
	{
		PostgresMainLongJmp();
	}
```

This allows us to handle catching exceptions manually. We did this for keeping code changes to a minimum and not having to set the jump every time we process a query. In keeping with the original code, setting the jump is only done once, on startup.

To handle the longjmp, we override the `siglongjmp()` call, which calls `exit()` (while keeping the environment alive) with a specific code. We push this to the frontend to allow it to call `PostgresMainLongJmp()` and continue processing:

```
void EMSCRIPTEN_KEEPALIVE pgl_longjmp(jmp_buf env, int val) {
    if (is_pglite_active && memcmp(env, (void*)postgresmain_sigjmp_buf, sizeof(jmp_buf)) == 0) {
        // reset this as it is expected
        if (!ignore_till_sync)
		    send_ready_for_query = true;	/* initially, or after error */
        exit(POSTGRES_MAIN_LONGJMP);
    }
    longjmp(env, val);
}
```

This is also what happens in a regular Postgres implementation, albeit with a bit more indirection.

## Data exchange

To exchange data between the backend and frontend of PGlite, we override the libc socket functions:

```
...
-Drecv=pgl_recv -Dsend=pgl_send -Dconnect=pgl_connect -Dsetsockopt=pgl_setsockopt -Dgetsockopt=pgl_getsockopt -Dgetsockname=pgl_getsockname
```

See `build-pglite.sh`. This makes it appear as if PostgreSQL is sending data to a socket, but we just push the data to the frontend:

```
ssize_t EMSCRIPTEN_KEEPALIVE pgl_send(int __fd, const void *__buf, size_t __n, int __flags) {
	ssize_t wrote = pgl_write(__buf, __n);
	return wrote;
}
```

Here `pgl_write()` is a callback function that should be set at startup.

## Emscripten specific flags

We aim to keep all wasm/emscripten-specific flags outside of the main code. Most of these are consolidated in `build-pglite.sh`, with some exceptions spread throughout the code, like in `pgxs.mk`:

```
ifeq ($(PORTNAME), emscripten)
...
endif
```

## Other

As seen in `pglite/src/pglitec/pglitec.c` we override other `libc` functions for various reasons:

- `popen`, `system`, `popen` to intercept calls to create/close processes (eg `initdb` will call these multiple times to set up a new database).
- `munmap` because this atm [it is not supported by emscripten](https://github.com/emscripten-core/emscripten/issues/21816).
- `freopen` for redirecting the stdin<->stdout communication between `initdb` and PGlite.
- `atexit` to be able to run the cleanup code upon closing PGlite (ie when calling `pg.close()`)
- various functions related to user rights, which are not supported by emscripten or the returned values do not match what Postgres is expecting.
- various other functions for which the return value and/or behavior is not relevant in a WASM environment

Overriding these functions allows us to limit the intervention on the original code and restrict them to a single file.

There are a few changes needed to make PostgreSQL "single-user mode" to behave as a normal backend (see `pgl_startPGlite` inside `postgres-pglite/src/backend/tcop/postgres.c`) or to allow us to handle the faked socket.
