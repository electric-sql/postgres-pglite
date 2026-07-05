#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/pglite (PGlite host hook mechanisms)
#
# IDENTIFICATION
#    src/backend/pglite/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/pglite
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	pgl_apply.o \
	pgl_commit_gate.o \
	pgl_readset.o \
	pgl_reset.o \
	pgl_sequence_lease.o \
	pgl_walscan.o

include $(top_srcdir)/src/backend/common.mk
