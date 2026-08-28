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
		pgmq/pgmq-extension \
		pg_bigm

# Every other extension here includes PGXS unconditionally. pg_bigm gates it
# behind USE_PGXS and otherwise assumes it sits in the postgres tree as
# contrib/pg_bigm, which resolves contrib-global.mk to a path that does not
# exist from here. Set the flag for its build and its packaging step only.
all-pg_bigm-recurse pg_bigm.tar.gz: export USE_PGXS = 1

prefix ?= /pglite
EXTENSIONS_BUILD_ROOT := /tmp/extensions/build
ARCHIVE_DIR := /pglite/extensions/other

EXTENSIONS := $(SUBDIRS)
EXTENSIONS += postgis

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