#ifndef PGLITE_ROOT_PLACEMENT_H
#define PGLITE_ROOT_PLACEMENT_H

#include "pglitec.h"
#include "postgres.h"
#include "storage/dsm.h"
#include "utils/dsa.h"

#ifndef __PGLITE_POSTMASTER__
#error "Root placement requires the postmaster memory ABI"
#endif

#ifdef PGLITE_MEMORY_ROOT_DSA
extern dsa_area *pgl_root_dsa_create_ext(int tranche_id, size_t initial_size,
                                        size_t maximum_size);

#define dsa_create_ext(tranche_id, initial_size, maximum_size) \
	pgl_root_dsa_create_ext(tranche_id, initial_size, maximum_size)
#endif

#ifdef PGLITE_MEMORY_ROOT_DSM
#define dsm_create(size, flags) \
	dsm_create_in_domain(size, flags, PGL_SHARED_MEMORY_ROOT)
#endif

#ifdef PGLITE_LOADER_STAT
#define stat(...) pgl_dlopen_stat(__VA_ARGS__)
#endif

#ifdef PGLITE_BACKEND_SIGNALS
#define sigaction(...) pgl_sigaction(__VA_ARGS__)
#endif

#ifdef PGLITE_BACKEND_EXECUTABLE
#define find_other_exec(argv0, target, version, result) \
	pgl_find_other_exec(argv0, target, version, result, MAXPGPATH)
#endif

#endif
