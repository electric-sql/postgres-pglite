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

LICENSE_PATTERNS := -iname 'LICENSE*' -o -iname 'NOTICE*' -o -iname 'COPYING*'

# Copy license/notice files of extension $(1) into $(2).
# Falls back to the parent directory (e.g. pgmq/pgmq-extension -> pgmq).
define install-license-files
	@echo "=== Adding license files for $(1) ==="
	mkdir -p $(2)
	dir=$(1); \
	files=$$(find $$dir -maxdepth 1 -type f \( $(LICENSE_PATTERNS) \)); \
	if [ -z "$$files" ]; then \
		dir=$$(dirname $$dir); \
		files=$$(find $$dir -maxdepth 1 -type f \( $(LICENSE_PATTERNS) \)); \
	fi; \
	if [ -n "$$files" ]; then cp $$files $(2)/; \
	else echo "WARNING: no license files found for $(1)"; fi
endef

# Default target: build tarballs for all contribs
dist: $(addsuffix .tar.gz,$(EXTENSIONS))

# Pattern rule: build $(EXT).tar.gz for each extra extension
%.tar.gz:
	@echo "=== Staging $* ==="
	rm -rf $(EXTENSIONS_BUILD_ROOT)/$*
	bash -c 'mkdir -p $(EXTENSIONS_BUILD_ROOT)/$*/$(prefix)/{bin,lib,share/extension,share/doc,share/postgresql/extension,share/postgresql/tsearch_data,include,share/postgresql/contrib/postgis-3.6}'
	$(MAKE) -C $* install DESTDIR=$(EXTENSIONS_BUILD_ROOT)/$*
	$(call install-license-files,$*,$(EXTENSIONS_BUILD_ROOT)/$*/$(prefix)/share/doc/$(notdir $*))
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
	$(call install-license-files,age,$(EXTENSIONS_BUILD_ROOT)/age/$(prefix)/share/doc/age)
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