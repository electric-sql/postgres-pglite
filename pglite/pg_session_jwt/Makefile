# Minimal PGXS-style shim for PGlite builds.
#
# Upstream pg_session_jwt is pgrx-based and doesn't ship a Makefile compatible
# with PGlite's backend build (which expects to `make install` each extension
# directory). This shim installs a fallback-mode-only SQL implementation that
# uses PostgREST-compatible `request.jwt.claims`.

EXTENSION = pg_session_jwt
EXTVERSION = 0.4.0

prefix ?= /install/pglite

EXTENSION_DIR = $(DESTDIR)$(prefix)/share/postgresql/extension

.PHONY: all install

all:
	@echo "$(EXTENSION): nothing to build (SQL-only shim)"

install:
	mkdir -p $(EXTENSION_DIR)
	printf "%s\n" \
		"comment = 'pg_session_jwt (PGlite shim): JWT session helpers using request.jwt.claims'" \
		"default_version = '$(EXTVERSION)'" \
		"relocatable = false" \
		"trusted = true" \
		> $(EXTENSION_DIR)/$(EXTENSION).control
	cp -f pglite/$(EXTENSION)--$(EXTVERSION).sql $(EXTENSION_DIR)/
