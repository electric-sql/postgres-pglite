/*
 * libpq normally obtains these public symbols from its shared-library link.
 * The regression-only postmaster artifact links libpq.a into the backend, so
 * route the public frontend ABI to PostgreSQL's already-linked private copy.
 */

#include <stdbool.h>

extern int pg_char_to_encoding_private(const char *name);
extern const char *pg_encoding_to_char_private(int encoding);

bool
pgl_static_libpq_link_canary_is_frontend(void)
{
	return true;
}

int
pg_char_to_encoding(const char *name)
{
	return pg_char_to_encoding_private(name);
}

const char *
pg_encoding_to_char(int encoding)
{
	return pg_encoding_to_char_private(encoding);
}
