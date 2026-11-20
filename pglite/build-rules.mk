# Supplemental PGlite targets; PostgreSQL supplies the normal object list.
postgres: $(PGLITE_BACKEND_RUNTIME_OBJECT)
pglite: $(PGLITE_EXTRA_OBJS)

.PHONY: pglite-owned-exports
pglite-owned-exports:
	$(MKDIR_P) /install/pglite
	cat $(top_builddir)/src/interfaces/libpq/exports.list \
	$(addprefix $(LIB_EXPORTS_DIR)/,libossp-uuid.exports libssl.exports libcrypto.exports libjson-c.exports libdeflate.exports libtiff.exports libsqlite3.exports libproj.exports libgeos.exports libgdal.exports) \
	| sort -u > /install/pglite/excluded.imports.gen
	cat $(DESTDIR)$(emscripten_extension_imports_dir)/*.imports '$(top_builddir)/pglite/static/included.pglite.exports' \
	| sort -u | grep -v -E '^(invoke_|__indirect_function_table)' \
	| grep -Fvx -f /install/pglite/excluded.imports.gen | awk NF \
	| sed 's/^/_/' > /install/pglite/exported_functions.txt
