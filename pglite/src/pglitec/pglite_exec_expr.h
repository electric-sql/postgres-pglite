#ifndef PGLITE_EXEC_EXPR_H
#define PGLITE_EXEC_EXPR_H

#include "pglite_provenance.h"

#ifdef __PGLITE_MULTI_MEMORY__
/* Reassert private control after each dispatch; slot payloads can be shared. */
#define EEO_PRIVATE_OP()		(op = PGLITE_PRIVATE_POINTER(op))
#undef EEO_DISPATCH
#if defined(EEO_USE_COMPUTED_GOTO)
#define EEO_DISPATCH() \
	do { \
		EEO_PRIVATE_OP(); \
		goto *((void *) op->opcode); \
	} while (0)
#else
#define EEO_DISPATCH() \
	do { \
		EEO_PRIVATE_OP(); \
		goto starteval; \
	} while (0)
#endif
#define EEO_FETCH_VAR(slot, attnum) \
	do { \
		TupleTableSlot *private_slot = PGLITE_PRIVATE_POINTER(slot); \
		Datum *private_values = \
			PGLITE_PRIVATE_POINTER(private_slot->tts_values); \
		bool *private_isnull = \
			PGLITE_PRIVATE_POINTER(private_slot->tts_isnull); \
		Datum *private_value = \
			PGLITE_PRIVATE_POINTER(private_values + (attnum)); \
		bool *private_null = \
			PGLITE_PRIVATE_POINTER(private_isnull + (attnum)); \
		Datum *private_resvalue = PGLITE_PRIVATE_POINTER(op->resvalue); \
		bool *private_resnull = PGLITE_PRIVATE_POINTER(op->resnull); \
		*private_resvalue = *private_value; \
		*private_resnull = *private_null; \
	} while (0)
#else
#define EEO_PRIVATE_OP()		((void) 0)
#define EEO_FETCH_VAR(slot, attnum) \
	do { \
		*op->resvalue = (slot)->tts_values[attnum]; \
		*op->resnull = (slot)->tts_isnull[attnum]; \
	} while (0)
#endif

#endif
