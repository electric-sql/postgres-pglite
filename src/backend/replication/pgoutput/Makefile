#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/replication/pgoutput
#
# IDENTIFICATION
#    src/backend/replication/pgoutput
#
#-------------------------------------------------------------------------

subdir = src/backend/replication/pgoutput
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	pgoutput.o
PGFILEDESC = "pgoutput - standard logical replication output plugin"
NAME = pgoutput

all: all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

ifeq ($(PORTNAME), emscripten)
# pgoutput.so is loaded at runtime as an emscripten side module, but unlike
# pgxs-built extensions it is linked from src/backend, so its imports from the
# core module are not collected automatically. Without this, symbols used only
# by pgoutput (e.g. logicalrep_write_*) are dead-code-eliminated from the main
# module (-sMAIN_MODULE=2) and logical decoding fails at runtime with a call
# to an undefined table entry. Generate the .imports file here so the
# pglite-exported-functions target keeps these symbols alive.
install: install-imports

.PHONY: install-imports
install-imports: all
	$(MKDIR_P) '$(DESTDIR)$(emscripten_extension_imports_dir)'
	find . -name "*.o" -exec $(LLVM_NM) --undefined-only {} \; | awk '{print $$2}' | sed '/^$$/d' | sort -u > '$(NAME).undef.txt'
	find . -type f \( -name "*.o" -o -name "*.so" \) -exec $(LLVM_NM) --defined-only {} \; | awk '$$2 ~ /^[TDB]$$/ {print $$3}' | sed '/^$$/d' | sort -u > '$(NAME).defs.txt'
	comm -23 '$(NAME).undef.txt' '$(NAME).defs.txt' > '$(DESTDIR)$(emscripten_extension_imports_dir)/$(NAME).imports'
endif

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean: clean-lib
	rm -f $(OBJS) $(NAME).undef.txt $(NAME).defs.txt
