#if defined(__PGLITE__)

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
// TODO: an include for libpglite
#endif

typedef ssize_t (*pglite_system_t)(void *command);
pglite_system_t pglite_system = NULL;

void EMSCRIPTEN_KEEPALIVE
pgl_set_system_fn(pglite_system_t system_fn) {
    pglite_system = system_fn;
}

int system(const char *command) {
    if (pglite_system) {
        return pglite_system(command);
    }
    return 123;
}

#endif