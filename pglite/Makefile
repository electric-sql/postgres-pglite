# Packages other extensions into individual tar.gz archives

top_builddir = ..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		pg_ivm \
		vector \
		pgtap \
		pg_uuidv7 \
		pg_hashids

prefix ?= /install/pglite
EXTENSIONS_BUILD_ROOT := /tmp/extensions/build
ARCHIVE_DIR := /install/pglite/extensions
POSTGIS_ARCHIVE_DIR := /install/pglite/extensions/postgis

EXTENSIONS := $(SUBDIRS)
EXTENSIONS += postgis

# Default target: build tarballs for all contribs
dist: $(addsuffix .tar.gz,$(EXTENSIONS))

# Pattern rule: build $(EXT).tar.gz for each extra extension
# 	rm -rf $(EXTENSIONS_BUILD_ROOT)/$*
%.tar.gz:
	@echo "=== Staging $* ==="
	bash -c 'mkdir -p $(EXTENSIONS_BUILD_ROOT)/$*/$(prefix)/{bin,lib,share/extension,share/doc,share/postgresql/extension,share/postgresql/tsearch_data,include,share/postgresql/contrib/postgis-3.6}'
	$(MAKE) -C $* install DESTDIR=$(EXTENSIONS_BUILD_ROOT)/$*
	@echo "=== Packaging $* ==="
	mkdir -p $(ARCHIVE_DIR)
	cd $(EXTENSIONS_BUILD_ROOT)/$*/$(prefix) && \
	files=$$(find . -type f -o -type l | sed 's|^\./||') && \
	tar -czf $(ARCHIVE_DIR)/$*.tar.gz $$files

dist-postgis:
	@echo "=== Moving postgis ==="
	mkdir -p $(POSTGIS_ARCHIVE_DIR) && mv $(ARCHIVE_DIR)/postgis.tar.gz $(POSTGIS_ARCHIVE_DIR)

.PHONY: dist

archive_clean:
	@echo "=== Cleaning $* ==="
	rm -rf $(EXTENSIONS_BUILD_ROOT)/$*
	rm -rf $(ARCHIVE_DIR)

$(recurse)
