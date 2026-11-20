#ifndef PGLITE_TUPLE_DEFORM_H
#define PGLITE_TUPLE_DEFORM_H

#ifdef __PGLITE_MULTI_MEMORY__

/* Separate private-payload wrappers preserve slow/null specialization.
 * Shared payloads keep the generic path in the caller. */
static pg_noinline int
slot_deform_heap_tuple_private_fast_no_nulls(TupleTableSlot *slot,
											 HeapTupleHeader tup,
											 int attnum, int natts,
											 uint32 *offp, bool *slowp)
{
	tup = PGLITE_PRIVATE_POINTER(tup);
	return slot_deform_heap_tuple_internal(slot, tup, attnum, natts,
											false, false, true, offp, slowp);
}

static pg_noinline int
slot_deform_heap_tuple_private_fast_nulls(TupleTableSlot *slot,
										 HeapTupleHeader tup,
										 int attnum, int natts,
										 uint32 *offp, bool *slowp)
{
	tup = PGLITE_PRIVATE_POINTER(tup);
	return slot_deform_heap_tuple_internal(slot, tup, attnum, natts,
											false, true, true, offp, slowp);
}

static pg_noinline int
slot_deform_heap_tuple_private_slow(TupleTableSlot *slot,
								   HeapTupleHeader tup,
								   int attnum, int natts, bool hasnulls,
								   uint32 *offp, bool *slowp)
{
	tup = PGLITE_PRIVATE_POINTER(tup);
	return slot_deform_heap_tuple_internal(slot, tup, attnum, natts,
											true, hasnulls, true, offp, slowp);
}

/* Inputs are locals/constants; attnum is updated in place. */
#define PGLITE_DEFORM_HEAP_TUPLE(slot, tup, attnum, natts, slow, hasnulls, offp, slowp) \
	do { \
		if (PGLITE_POINTER_IS_PRIVATE(tup)) \
		{ \
			if (slow) \
				(attnum) = slot_deform_heap_tuple_private_slow(slot, tup, attnum, natts, hasnulls, offp, slowp); \
			else if (hasnulls) \
				(attnum) = slot_deform_heap_tuple_private_fast_nulls(slot, tup, attnum, natts, offp, slowp); \
			else \
				(attnum) = slot_deform_heap_tuple_private_fast_no_nulls(slot, tup, attnum, natts, offp, slowp); \
		} \
		else \
			(attnum) = slot_deform_heap_tuple_internal(slot, tup, attnum, natts, slow, hasnulls, false, offp, slowp); \
	} while (0)
#else
#define PGLITE_DEFORM_HEAP_TUPLE(slot, tup, attnum, natts, slow, hasnulls, offp, slowp) \
	((attnum) = slot_deform_heap_tuple_internal(slot, tup, attnum, natts, slow, hasnulls, false, offp, slowp))
#endif

#endif
