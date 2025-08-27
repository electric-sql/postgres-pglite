#pragma once


#include <sys/stat.h>


#include <stdio.h> // FILE
#include <stdlib.h>
#include <string.h>

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



