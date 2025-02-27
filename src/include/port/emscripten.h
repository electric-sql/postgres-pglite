/* src/include/port/emscripten.h */

#ifndef I_EMSCRIPTEN
#define I_EMSCRIPTEN

#if !defined(__cplusplus)
#include <emscripten.h>
#endif

#include "/tmp/pglite/include/wasm_common.h"


#define BOOT_END_MARK "build indices"
#define FD_BUFFER_MAX 16384


/*
 * and now popen will return predefined slot from a file list
 * as file handle in initdb.c
 */
#if defined(PG_INITDB) && !defined(PG_MAIN)
#define popen(command, mode) pg_popen(command, mode)
#include <stdio.h> // FILE+fprintf
extern FILE* IDB_PIPE_FP;
extern FILE* SOCKET_FILE;
extern int SOCKET_DATA;
extern int IDB_STAGE;
FILE *pg_popen(const char *command, const char *type) {
    if (IDB_STAGE>1) {
    	fprintf(stderr,"# popen[%s]\n", command);
    	return stderr;
    }

    if (!IDB_STAGE) {
        fprintf(stderr,"# popen[%s] (BOOT)\n", command);
        IDB_PIPE_FP = fopen( IDB_PIPE_BOOT, "w");
        IDB_STAGE = 1;
    } else {
        fprintf(stderr,"# popen[%s] (SINGLE)\n", command);
        IDB_PIPE_FP = fopen( IDB_PIPE_SINGLE, "w");
        IDB_STAGE = 2;
    }

    return IDB_PIPE_FP;

}
#endif // PG_INITDB


#endif // I_EMSCRIPTEN
