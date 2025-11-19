#if defined(__PGLITE__)

typedef ssize_t (*pglite_system)(void *command);
pglite_system_t pglite_system = NULL;

int system(const char *command) {
    if (pglite_system) {
        return pglite_system(command);
    }
    return 123;
}

#endif