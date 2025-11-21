#if defined(__PGLITE__)

#ifndef _PGLITE_SYSTEM_
#define _PGLITE_SYSTEM_

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

typedef FILE* (*pglite_popen_t)(const char *command, const char *mode);
pglite_popen_t pglite_popen = NULL;

void EMSCRIPTEN_KEEPALIVE
pgl_set_popen_fn(pglite_popen_t popen_fn) {
    pglite_popen = popen_fn;
}

FILE* EMSCRIPTEN_KEEPALIVE
popen(const char *command, const char *mode) {
    if (pglite_popen) {
        return pglite_popen(command);
    }
    return NULL;
}

#endif
#endif