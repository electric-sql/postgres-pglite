top_builddir = ../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		pg_ivm \
		vector \
		pgtap \
		pg_uuidv7 \
		pg_hashids \
		pg_textsearch

prefix ?= /pglite
EXTENSIONS_BUILD_ROOT := /tmp/extensions/build
ARCHIVE_DIR := /pglite/extensions
POSTGIS_ARCHIVE_DIR := /pglite/extensions/postgis

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
	tar -czf $(ARCHIVE_DIR)/$*.tar.gz $$files

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
# 	tar -C $(EXTENSIONS_BUILD_ROOT)/$*/$(prefix) -czf $(ARCHIVE_DIR)/$*.tar.gz .

dist-postgis:
	@echo "=== Adding proj data to postgis archive ==="
	gunzip $(ARCHIVE_DIR)/postgis.tar.gz
	tar -rf $(ARCHIVE_DIR)/postgis.tar -C /install/libs share/proj
	gzip $(ARCHIVE_DIR)/postgis.tar
	@echo "=== Moving postgis ==="
	mkdir -p $(POSTGIS_ARCHIVE_DIR) && mv $(ARCHIVE_DIR)/postgis.tar.gz $(POSTGIS_ARCHIVE_DIR)

.PHONY: dist

archive_clean:
	@echo "=== Cleaning $* ==="
	rm -rf $(EXTENSIONS_BUILD_ROOT)/$*
	rm -rf $(ARCHIVE_DIR)

$(recurse)