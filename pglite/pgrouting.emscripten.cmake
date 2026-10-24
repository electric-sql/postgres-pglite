# Injected into the pgRouting CMake project via -DCMAKE_PROJECT_INCLUDE (see build-pgrouting.sh).
# Emscripten's toolchain file disables shared libraries, which silently turns
# add_library(... SHARED ...) into a static archive. PGlite needs a SIDE_MODULE .so instead.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
set(CMAKE_SHARED_LIBRARY_PREFIX "lib")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
set(CMAKE_SHARED_MODULE_SUFFIX ".so")
