/*
 * PGlite multi-memory provenance markers.
 *
 * Calls are checked identities in an untransformed/debug build. The
 * multi-memory post-linker consumes them as explicit private-pointer proofs
 * and removes the calls from release artifacts.
 */
#ifndef PGLITE_PROVENANCE_H
#define PGLITE_PROVENANCE_H

extern void *pgl_private_pointer(void *pointer);

#define PGLITE_PRIVATE_POINTER(pointer) \
	((__typeof__(pointer)) pgl_private_pointer((void *) (pointer)))

/* Keep tag interpretation out of PostgreSQL source fences. */
#define PGLITE_POINTER_IS_PRIVATE(pointer) ((intptr_t) (pointer) > 0)

#endif
