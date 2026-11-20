#include "postgres.h"

#include <limits.h>
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/pg_sema.h"
#include "storage/shmem.h"
#include "pglitec.h"

/* Each semaphore lives entirely in shared memory, including its wait word. */
typedef struct PGSemaphoreData
{
	pg_atomic_uint32 count;
	pg_atomic_uint32 wake_sequence;
	char pad[PG_CACHE_LINE_SIZE - 2 * sizeof(pg_atomic_uint32)];
} PGSemaphoreData;

static PGSemaphore shared_semaphores;
static int semaphore_count;
static int semaphore_capacity;

Size
PGSemaphoreShmemSize(int max_semaphores)
{
	return mul_size(max_semaphores, sizeof(PGSemaphoreData));
}

void
PGReserveSemaphores(int max_semaphores)
{
	/* The shared-memory spinlock is not initialized at this point. */
	shared_semaphores = ShmemAllocUnlocked(PGSemaphoreShmemSize(max_semaphores));
	semaphore_count = 0;
	semaphore_capacity = max_semaphores;
}

PGSemaphore
PGSemaphoreCreate(void)
{
	PGSemaphore semaphore;

	Assert(!IsUnderPostmaster);
	if (semaphore_count >= semaphore_capacity)
		elog(PANIC, "too many semaphores created");
	semaphore = &shared_semaphores[semaphore_count++];
	pg_atomic_init_u32(&semaphore->count, 1);
	pg_atomic_init_u32(&semaphore->wake_sequence, 0);
	return semaphore;
}

void
PGSemaphoreReset(PGSemaphore semaphore)
{
	pg_atomic_write_u32(&semaphore->count, 0);
	pg_atomic_fetch_add_u32(&semaphore->wake_sequence, 1);
	(void) pgl_futex_wake(&semaphore->wake_sequence, INT_MAX);
}

void
PGSemaphoreLock(PGSemaphore semaphore)
{
	for (;;)
	{
		uint32 count = pg_atomic_read_u32(&semaphore->count);

		while (count > 0)
		{
			if (pg_atomic_compare_exchange_u32(&semaphore->count, &count, count - 1))
				return;
		}
		count = pg_atomic_read_u32(&semaphore->wake_sequence);
		if (pg_atomic_read_u32(&semaphore->count) == 0)
		{
			if (pgl_futex_wait(&semaphore->wake_sequence, count, -1) < 0 &&
				errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT)
				elog(FATAL, "PGlite semaphore wait failed: %m");
		}
		pgl_dispatch_pending_signals();
	}
}

void
PGSemaphoreUnlock(PGSemaphore semaphore)
{
	pg_atomic_fetch_add_u32(&semaphore->count, 1);
	pg_atomic_fetch_add_u32(&semaphore->wake_sequence, 1);
	if (pgl_futex_wake(&semaphore->wake_sequence, 1) < 0)
		elog(FATAL, "PGlite semaphore wake failed: %m");
}

bool
PGSemaphoreTryLock(PGSemaphore semaphore)
{
	uint32 count = pg_atomic_read_u32(&semaphore->count);

	while (count > 0)
	{
		if (pg_atomic_compare_exchange_u32(&semaphore->count, &count, count - 1))
			return true;
	}
	return false;
}
