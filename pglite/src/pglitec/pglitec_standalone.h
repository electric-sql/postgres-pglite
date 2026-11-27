/*-------------------------------------------------------------------------
 *
 * pglitec_standalone.h
 *	  Extensions linked into the standalone build
 *
 * NOTES
 *    The standalone build cannot load shared libraries, so extensions are
 *    linked into the module, and dlopen()/dlsym() look them up in
 *    pgl_static_libraries. build-pglite-standalone.sh generates that table.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGLITEC_STANDALONE_H
#define PGLITEC_STANDALONE_H

typedef struct pgl_static_symbol {
    const char *name;
    void *address;
} pgl_static_symbol;

typedef struct pgl_static_library {
    const char *name;                   /* file name without directory and suffix */
    const pgl_static_symbol *symbols;   /* terminated by a NULL name */
} pgl_static_library;

/* terminated by a NULL name */
extern const pgl_static_library pgl_static_libraries[];

#endif /* PGLITEC_STANDALONE_H */
