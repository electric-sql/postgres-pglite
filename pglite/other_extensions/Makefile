top_builddir = ../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		pg_ivm \
		vector \
		pgtap \
		age \
		pg_uuidv7 \
		pg_hashids \
		pg_textsearch \
		pgmq/pgmq-extension

prefix ?= /pglite
EXTENSIONS_BUILD_ROOT := /tmp/extensions/build
ARCHIVE_DIR := /pglite/extensions/other

EXTENSIONS := $(SUBDIRS)
EXTENSIONS += postgis

# NOTE: pg_partman is deliberately not in SUBDIRS: its default target would compile the
# background worker, which cannot run in WASM. It is built SQL-only via its own
# dist rule below with NO_BGW=1 (which is an officially supported pg_partman build mode).
EXTENSIONS += pg_partman

# Default target: build tarballs for all contribs
dist: $(addsuffix .tar.gz,$(EXTENSIONS))

# Pattern rule: build $(EXT).tar.gz for each extra extension
%.tar.gz:
	@echo "=== Staging $* ==="
	rm -rf $(EXTENSIONS_BUILD_ROOT)/$*
	bash -c 'mkdir -p $(EXTENSIONS_BUILD_ROOT)/$*/$(prefix)/{bin,lib,share/extension,share/doc,share/postgresql/extension,share/postgresql/tsearch_data,include,share/postgresql/contrib/postgis-3.6}'
	$(MAKE) -C $* install DESTDIR=$(EXTENSIONS_BUILD_ROOT)/$*
	@echo "=== Packaging $* ==="
	mkdir -p $(ARCHIVE_DIR)
	cd $(EXTENSIONS_BUILD_ROOT)/$*/$(prefix) && \
	files=$$(find . -type f -o -type l | sed 's|^\./||') && \
	tar -czf $(ARCHIVE_DIR)/$$(basename $*)\.tar.gz $$files

# AGE requires SIZEOF_DATUM=4 for 32-bit WASM compatibility (strips PASSEDBYVALUE from graphid)
age.tar.gz:
	@echo "=== Staging age (with 32-bit support) ==="
	rm -rf $(EXTENSIONS_BUILD_ROOT)/age
	bash -c 'mkdir -p $(EXTENSIONS_BUILD_ROOT)/age/$(prefix)/{bin,lib,share/extension,share/doc,share/postgresql/extension,share/postgresql/tsearch_data,include}'
	rm -f age/age--*.sql
	$(MAKE) -C age install DESTDIR=$(EXTENSIONS_BUILD_ROOT)/age SIZEOF_DATUM=4
	@echo "=== Packaging age ==="
	mkdir -p $(ARCHIVE_DIR)
	cd $(EXTENSIONS_BUILD_ROOT)/age/$(prefix) && \
	files=$$(find . -type f -o -type l | sed 's|^\./||') && \
	tar -czf $(ARCHIVE_DIR)/age.tar.gz $$files

# pg_partman's core is pure SQL/PLpgSQL; NO_BGW=1 skips its background worker C module
pg_partman.tar.gz:
	@echo "=== Staging pg_partman (SQL-only, NO_BGW=1) ==="
	rm -rf $(EXTENSIONS_BUILD_ROOT)/pg_partman
	bash -c 'mkdir -p $(EXTENSIONS_BUILD_ROOT)/pg_partman/$(prefix)/{bin,lib,share/extension,share/doc,share/postgresql/extension}'
	$(MAKE) -C pg_partman NO_BGW=1 install DESTDIR=$(EXTENSIONS_BUILD_ROOT)/pg_partman
	@echo "=== Packaging pg_partman ==="
	mkdir -p $(ARCHIVE_DIR)
	cd $(EXTENSIONS_BUILD_ROOT)/pg_partman/$(prefix) && \
	files=$$(find . -type f -o -type l | sed 's|^\./||') && \
	tar -czf $(ARCHIVE_DIR)/pg_partman.tar.gz $$files

dist-postgis:
	@echo "=== Adding proj data to postgis archive ==="
	gunzip $(ARCHIVE_DIR)/postgis.tar.gz
	tar -rf $(ARCHIVE_DIR)/postgis.tar -C /install/libs share/proj
	gzip $(ARCHIVE_DIR)/postgis.tar

.PHONY: dist

archive_clean:
	@echo "=== Cleaning $* ==="
	rm -rf $(EXTENSIONS_BUILD_ROOT)/$*
	rm -rf $(ARCHIVE_DIR)

$(recurse)
