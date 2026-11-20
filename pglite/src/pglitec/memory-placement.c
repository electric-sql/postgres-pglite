#include "postgres.h"
#include "storage/dsm.h"
#include "utils/dsa.h"
#include "pglitec.h"
#include <emscripten/emscripten.h>

/* Restore placement on success and PostgreSQL ERROR. */
dsm_segment *
dsm_create_in_domain(Size size, int flags, PglSharedMemoryDomain domain)
{
	dsm_segment *volatile segment = NULL;
	PglSharedMemoryDomain previous_domain;

	previous_domain = pgl_shm_domain_push(domain);
	PG_TRY();
	{
		segment = dsm_create(size, flags);
	}
	PG_FINALLY();
	{
		pgl_shm_domain_pop(previous_domain);
	}
	PG_END_TRY();
	return (dsm_segment *) segment;
}

#ifdef __PGLITE_POSTMASTER__
/* Keep PostgreSQL's error handler in the same module as its longjmp target. */
dsa_area *EMSCRIPTEN_KEEPALIVE
pgl_root_dsa_create_ext(int tranche_id, size_t initial_size, size_t maximum_size)
{
	dsa_area *volatile area = NULL;
	PglSharedMemoryDomain previous_domain;

	previous_domain = pgl_shm_domain_push(PGL_SHARED_MEMORY_ROOT);
	PG_TRY();
	{
		area = dsa_create_ext(tranche_id, initial_size, maximum_size);
	}
	PG_FINALLY();
	{
		pgl_shm_domain_pop(previous_domain);
	}
	PG_END_TRY();
	return (dsa_area *) area;
}
#endif
