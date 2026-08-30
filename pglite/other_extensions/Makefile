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
		plpgsql_check

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

# plpgsql_check's upstream Makefile hardcodes -g; strip the debug info at
# packaging time (instead of patching the submodule) to keep the .so small
# and avoid shipping DWARF sections.
plpgsql_check.tar.gz:
	@echo "=== Staging plpgsql_check ==="
	rm -rf $(EXTENSIONS_BUILD_ROOT)/plpgsql_check
	bash -c 'mkdir -p $(EXTENSIONS_BUILD_ROOT)/plpgsql_check/$(prefix)/{bin,lib,share/extension,share/doc,share/postgresql/extension,share/postgresql/tsearch_data,include}'
	$(MAKE) -C plpgsql_check install DESTDIR=$(EXTENSIONS_BUILD_ROOT)/plpgsql_check
	emstrip --strip-debug $(EXTENSIONS_BUILD_ROOT)/plpgsql_check/$(prefix)/lib/postgresql/plpgsql_check.so
	@echo "=== Packaging plpgsql_check ==="
	mkdir -p $(ARCHIVE_DIR)
	cd $(EXTENSIONS_BUILD_ROOT)/plpgsql_check/$(prefix) && \
	files=$$(find . -type f -o -type l | sed 's|^\./||') && \
	tar -czf $(ARCHIVE_DIR)/plpgsql_check.tar.gz $$files

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