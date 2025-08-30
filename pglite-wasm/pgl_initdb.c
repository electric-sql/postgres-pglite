#pragma once

#include <stdio.h> // FILE+fprintf
#include <stdlib.h>
#include <string.h>
#ifndef PGL_INITDB_MAIN
#define PGL_INITDB_MAIN
#endif
#include <setjmp.h>

/*
 * and now popen will return predefined slot from a file list
 * as file handle in initdb.c
 */



/*
 * popen is routed via pg_popen to stderr or a IDB_PIPE_* file
 * link a pclose replacement when we are in exec.c ( PG_EXEC defined )
 */

extern FILE * pgl_popen(const char *command, const char *type);
#define popen(command, mode) pgl_popen(command, mode)
// #define popen_check(command, mode) pgl_popen(command, mode)

extern int pgl_pclose(FILE *stream);
#define pclose(stream) pgl_pclose(stream)
#define pclose_check(stream) pgl_pclose(stream)


int
pg_chmod(const char * path, int mode_t) {
    return 0;
}

#ifdef FRONTEND
#undef FRONTEND
#endif

#define FRONTEND
#   include "common/logging.c"
#undef FRONTEND


#include "interfaces/libpq/pqexpbuffer.c"

// On Android/mobile glue, ensure initdb does not attempt POSIX shm APIs
#undef HAVE_SHM_OPEN
#undef HAVE_SHM_UNLINK


#define sync_pgdata(...)
#define icu_language_tag(loc_str) icu_language_tag_idb(loc_str)
#define icu_validate_locale(loc_str) icu_validate_locale_idb(loc_str)


#ifdef PGL_MOBILE
// Force initdb to use our mobile discovery functions instead of probing real binaries
#define find_other_exec find_other_exec_mobile
#define get_share_path  get_share_path_mobile

static int find_other_exec_mobile(const char *argv0, const char *target, const char *versionstr, char *retpath) {
    (void)argv0; (void)versionstr;
    const char* prefix = getenv("PREFIX");
    if (!prefix || !*prefix) prefix = getenv("ANDROID_DATA_DIR");
    if (!prefix || !*prefix) prefix = "/data/local/tmp/pglite";
    const char* tgt = target && *target ? target : "postgres";
    snprintf(retpath, MAXPGPATH, "%s/bin/%s", prefix, tgt);
    return 0; // success
}

static void get_share_path_mobile(const char *my_exec_path, char *ret_path) {
    (void)my_exec_path;
    const char* conf = getenv("PGSYSCONFDIR");
#ifdef __APPLE__
    const char* runtime = getenv("IOS_RUNTIME_DIR");
#else
    const char* runtime = getenv("ANDROID_RUNTIME_DIR");
#endif
    // Candidates in order: PGSYSCONFDIR/share/postgresql, runtime/share/postgresql, runtime/postgresql
    if (conf && *conf) {
        snprintf(ret_path, MAXPGPATH, "%s/share/postgresql", conf);
        return;
    }
    if (runtime && *runtime) {
        // Prefer share/postgresql but accept postgresql fallback
        char cand[MAXPGPATH];
        snprintf(cand, sizeof(cand), "%s/share/postgresql", runtime);
        // Don't check for existence here; initdb will validate inputs shortly
        snprintf(ret_path, MAXPGPATH, "%s", cand);
        return;
    }
    // Last resort
    snprintf(ret_path, MAXPGPATH, "/data/local/tmp/pglite/share/postgresql");
}
#endif

#ifdef PGL_CATCH_EXIT
// Catch initdb's exit() and convert into a longjmp back to pgl_initdb_safe
static jmp_buf g_initdb_jmp;
static int g_initdb_status = 0;
static void pglite_initdb_exit(int code) {
    g_initdb_status = code;
    longjmp(g_initdb_jmp, 1);
}
#define exit(code) pglite_initdb_exit(code)
#endif

#include "bin/initdb/initdb.c"

#ifdef PGL_CATCH_EXIT
// Safe wrapper that prevents process termination on initdb failures
int pgl_initdb_safe(void) {
    g_initdb_status = 0;
    if (setjmp(g_initdb_jmp) == 0) {
        (void)pgl_initdb_main();
        return 0;
    }
    return g_initdb_status ? g_initdb_status : -1;
}
#endif

void use_socketfile(void) {
    is_repl = true;
    is_embed = false;
}



