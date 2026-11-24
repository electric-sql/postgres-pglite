/*-------------------------------------------------------------------------
 *
 * pglitec_heap.c
 *	  Keep the heap of a process inside its part of the common memory
 *
 * NOTES
 *    Linked with -Wl,--wrap=sbrk: every call to sbrk(), including the ones
 *    made by malloc, lands here first.
 *
 *    When several Postgres processes live in one WebAssembly.Memory, each
 *    has its own address range (see OS in processUtils.ts). The stock sbrk()
 *    only checks against the size of the whole memory, so a growing heap
 *    would silently run into the range of the next process.
 *
 *    Separate object file: only pulled into links using --wrap=sbrk.
 *
 *-------------------------------------------------------------------------
 */

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

extern void *__real_sbrk(intptr_t increment);

/* first address the heap may not use; 0 = no limit */
static uintptr_t pgl_heap_limit = 0;

uintptr_t EMSCRIPTEN_KEEPALIVE
pgl_set_heap_limit(uintptr_t limit)
{
	uintptr_t	prev = pgl_heap_limit;

	pgl_heap_limit = limit;
	return prev;
}

void *
__wrap_sbrk(intptr_t increment)
{
	if (pgl_heap_limit != 0 && increment > 0)
	{
		uintptr_t	cur = (uintptr_t) __real_sbrk(0);

		if (cur > pgl_heap_limit ||
			(uintptr_t) increment > pgl_heap_limit - cur)
		{
			errno = ENOMEM;
			return (void *) -1;
		}
	}
	return __real_sbrk(increment);
}
