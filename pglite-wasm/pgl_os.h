#pragma once


#include <sys/stat.h>


#include <stdio.h> // FILE
#include <stdlib.h>
#include <string.h>

// Unified logging macros for all platforms
#ifdef PGL_MOBILE
  #ifdef __ANDROID__
    #include <android/log.h>
    #define PGL_LOG(level, ...) __android_log_print(level, "PGLite", __VA_ARGS__)
    #define PGL_LOG_INFO(...) __android_log_print(ANDROID_LOG_INFO, "PGLite", __VA_ARGS__)
    #define PGL_LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "PGLite", __VA_ARGS__)
    #define PGL_LOG_WARN(...) __android_log_print(ANDROID_LOG_WARN, "PGLite", __VA_ARGS__)
  #else // iOS and other mobile
    #define PGL_LOG(level, ...) do { \
      fprintf(stderr, "[PGLite] "); \
      fprintf(stderr, __VA_ARGS__); \
      fprintf(stderr, "\n"); \
      fflush(stderr); \
    } while(0)
    #define PGL_LOG_INFO(...) PGL_LOG(0, __VA_ARGS__)
    #define PGL_LOG_ERROR(...) PGL_LOG(0, __VA_ARGS__)
    #define PGL_LOG_WARN(...) PGL_LOG(0, __VA_ARGS__)
  #endif
#else // WASM
  #define PGL_LOG(level, ...) fprintf(stderr, __VA_ARGS__)
  #define PGL_LOG_INFO(...) fprintf(stderr, __VA_ARGS__)
  #define PGL_LOG_ERROR(...) fprintf(stderr, __VA_ARGS__)
  #define PGL_LOG_WARN(...) fprintf(stderr, __VA_ARGS__)
#endif

// Debug logging controlled by PGDEBUG flag (like PDEBUG in WASM)
#if PGDEBUG
  #define PGL_DEBUG(...) PGL_LOG_INFO(__VA_ARGS__)
#else
  #define PGL_DEBUG(...) // no-op
#endif

FILE* IDB_PIPE_FP = NULL;
int IDB_STAGE = 0;


/*
 * and now popen will return predefined slot from a file list
 * as file handle in initdb.c
 */

#ifdef PGL_MOBILE
static const char* get_env_or(const char* k, const char* d) { const char* v = getenv(k); return (v && *v) ? v : d; }
static void build_pipe_path(int stage, char* out, size_t outsz) {
  const char* runtime = getenv("ANDROID_RUNTIME_DIR");
#ifdef __APPLE__
  if (!runtime || !*runtime) runtime = getenv("IOS_RUNTIME_DIR");
#endif
  const char* base = (runtime && *runtime) ? runtime : get_env_or("PGDATA", "pglite/pgdata");
  const char* fname = stage==0 ? "initdb.boot.txt" : "initdb.single.txt";
  snprintf(out, outsz, "%s/%s", base, fname);
}
// Expose for readers (pg_main.c) to reopen the same files we wrote via pgl_popen
void pgl_get_pipe_path(int stage, char* out, size_t outsz) { build_pipe_path(stage, out, outsz); }
#endif

FILE *pgl_popen(const char *command, const char *type) {
    (void)type;
    if (IDB_STAGE>1) {
    	fprintf(stderr,"# popen[%s]\n", command);
    	return stderr;
    }

    if (!IDB_STAGE) {
        fprintf(stderr,"# popen[%s] (BOOT)\n", command);
    #ifdef PGL_MOBILE
        char path[1024]; build_pipe_path(0, path, sizeof(path));
        IDB_PIPE_FP = fopen(path, "w");
        fprintf(stderr, "# pgl_popen BOOT file=%s\n", path);
    #else
        IDB_PIPE_FP = fopen( IDB_PIPE_BOOT, "w");
        fprintf(stderr, "# pgl_popen BOOT file=%s\n", IDB_PIPE_BOOT);
    #endif
        IDB_STAGE = 1;
    } else {
        fprintf(stderr,"# popen[%s] (SINGLE)\n", command);
    #ifdef PGL_MOBILE
        char path[1024]; build_pipe_path(1, path, sizeof(path));
        IDB_PIPE_FP = fopen(path, "w");
        fprintf(stderr, "# pgl_popen SINGLE file=%s\n", path);
    #else
        IDB_PIPE_FP = fopen( IDB_PIPE_SINGLE, "w");
        fprintf(stderr, "# pgl_popen SINGLE file=%s\n", IDB_PIPE_SINGLE);
    #endif
        IDB_STAGE = 2;
    }

    return IDB_PIPE_FP;
}

#define popen(command, mode) pgl_popen(command, mode)

int
pgl_pclose(FILE *stream) {
    (void)stream;
    if (IDB_STAGE==1)
        fprintf(stderr,"# pg_pclose(BOOT) 133:" __FILE__ "\n");
    if (IDB_STAGE==2)
        fprintf(stderr,"# pg_pclose(SINGLE) 135:" __FILE__ "\n");

    if (IDB_PIPE_FP) {
        fflush(IDB_PIPE_FP);
        fclose(IDB_PIPE_FP);
        IDB_PIPE_FP = NULL;
    }
    return 0;
}



