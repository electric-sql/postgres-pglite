#ifndef PGLITE_TUPLE_ACCESS_H
#define PGLITE_TUPLE_ACCESS_H

#include "pglite_provenance.h"

/* Output arguments are local variables in the deformation loop. */
#ifdef __PGLITE_MULTI_MEMORY__
#define PGLITE_SET_TUPLE_PAYLOAD_POINTER(result, pointer, payload_private) \
	do { \
		(result) = (pointer); \
		if (payload_private) \
			(result) = PGLITE_PRIVATE_POINTER(result); \
	} while (0)
#define PGLITE_TEST_TUPLE_NULL(result, attnum, bitmap, payload_private) \
	do { \
		if (payload_private) \
		{ \
			bits8 *bitmap_byte = PGLITE_PRIVATE_POINTER((bitmap) + ((attnum) >> 3)); \
			(result) = !(*bitmap_byte & (1 << ((attnum) & 0x07))); \
		} \
		else \
			(result) = att_isnull(attnum, bitmap); \
	} while (0)
#else
#define PGLITE_SET_TUPLE_PAYLOAD_POINTER(result, pointer, payload_private) \
	((result) = (pointer))
#define PGLITE_TEST_TUPLE_NULL(result, attnum, bitmap, payload_private) \
	((result) = att_isnull(attnum, bitmap))
#endif

#endif
