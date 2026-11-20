#ifndef PGLITE_MISCADMIN_H
#define PGLITE_MISCADMIN_H

#include_next "miscadmin.h"

#if defined(__PGLITE_POSTMASTER__) && !defined(FRONTEND)
extern void pgl_dispatch_pending_signals(void);
#undef INTERRUPTS_PENDING_CONDITION
#define INTERRUPTS_PENDING_CONDITION() \
	(pgl_dispatch_pending_signals(), unlikely(InterruptPending))
#endif

#endif
