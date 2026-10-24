/*
 * Force-included (-include) into C++ extension sources built with -fexceptions (see build-pgrouting.sh).
 *
 * With Emscripten (JS based) EH + SjLj, a longjmp thrown through a C++ function that has try/catch
 * is only rethrown if LLVM's WebAssemblyLowerEmscriptenEHSjLj pass enables SjLj handling for that
 * translation unit, which it only does when the TU itself uses setjmp/longjmp. Otherwise the longjmp
 * is silently dropped: an ereport(ERROR) raised inside the C++ code (ie by SPI) returns normally and
 * leaves Postgres' error state corrupted. Referencing longjmp here turns that handling on.
 */
#include <setjmp.h>

__attribute__((used)) static void
pgl_force_emscripten_sjlj(jmp_buf env)
{
	longjmp(env, 1);
}
