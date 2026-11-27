/*-------------------------------------------------------------------------
 *
 * pglitec_standalone.c
 *	  PGlite support for the standalone (non-JS) build
 *
 *
 *
 * NOTES
 *    The standalone build (see build-pglite-standalone.sh) produces a wasm
 *    module that runs on any WASI runtime (e.g. Wasmtime) without the
 *    emscripten JS glue. This file provides what the JS glue provides in the
 *    regular build: returning control to the host, host I/O, filesystem
 *    population, and stubs for libc functions emscripten implements in JS.
 *
 *    The host interface is documented in packages/pglite-standalone in the
 *    parent repository.
 *
 *-------------------------------------------------------------------------
 */

#if defined(PGLITE_STANDALONE)

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <emscripten/emscripten.h>

#include "pglitec_standalone.h"

/* ========== Returning control to the host ==========
*
* PGlite returns control to its host with exit(): exit(PGLITE_EXIT_ALIVE) once
* the backend is ready for queries or has handled a Terminate message, and
* exit(POSTGRES_MAIN_LONGJMP) on a top level error longjmp (see pglitec.c). In
* the regular build, the JS frontend catches the resulting ExitStatus exception
* and keeps the runtime alive. Here, every host entry point sets a jump buffer
* and exit() siglongjmps back to it, abandoning the C stack. This relies on
* native wasm exception handling (-sSUPPORT_LONGJMP=wasm).
*/
#define PGL_RETURNED (-1)

static sigjmp_buf pgl_host_return;
static volatile int pgl_host_exit_status;
static volatile int pgl_in_host_entry = 0;

void pgl_exit_to_host(int status) {
    /* outside of a host entry point there is nowhere to return to: really exit */
    if (!pgl_in_host_entry)
        exit(status);
    pgl_host_exit_status = status;
    siglongjmp(pgl_host_return, 1);
}

/* run call, returning the exit status from the enclosing function if it exits */
#define PGL_HOST_ENTRY(call) \
    do { \
        if (sigsetjmp(pgl_host_return, 0) != 0) { \
            pgl_in_host_entry = 0; \
            return pgl_host_exit_status; \
        } \
        pgl_in_host_entry = 1; \
        call; \
        pgl_in_host_entry = 0; \
    } while (0)

extern int main(int argc, char *argv[]);
/* weak: the frontend programs (initdb) link this file too, without the backend */
extern void PostgresMainLoopOnce(void) __attribute__((weak));
extern void PostgresMainLongJmp(void) __attribute__((weak));

/*
* Host entry points, wrapping the functions the JS frontend calls directly.
* pgl_call_main() returns main()'s return value or exit status, like the JS
* frontend's callMain(). The others return PGL_RETURNED (-1) if the call
* returned, and the exit status if it exited, like the JS frontend catching
* ExitStatus.
*/
int EMSCRIPTEN_KEEPALIVE pgl_call_main(int argc, char *argv[]) {
    int rc = 0;
    PGL_HOST_ENTRY(rc = main(argc, argv));
    return rc;
}

int EMSCRIPTEN_KEEPALIVE pgl_loop_once(void) {
    PGL_HOST_ENTRY(PostgresMainLoopOnce());
    return PGL_RETURNED;
}

int EMSCRIPTEN_KEEPALIVE pgl_longjmp_recover(void) {
    PGL_HOST_ENTRY(PostgresMainLongJmp());
    return PGL_RETURNED;
}

/* ========== Host I/O ==========
*
* The wire protocol read/write callbacks are plain wasm imports from the
* "pglite" module instead of JS functions added to the table.
*/
#define PGL_HOST_IMPORT(name) __attribute__((import_module("pglite"), import_name(#name)))

PGL_HOST_IMPORT(read) extern ssize_t pgl_host_read(void *buffer, size_t max_length);
PGL_HOST_IMPORT(write) extern ssize_t pgl_host_write(void *buffer, size_t length);

typedef ssize_t (*pgl_read_t)(void *buffer, size_t max_length);
typedef ssize_t (*pgl_write_t)(void *buffer, size_t length);
extern void pgl_set_rw_cbs(pgl_read_t read_cb, pgl_write_t write_cb);

__attribute__((constructor))
static void pgl_standalone_init(void) {
    pgl_set_rw_cbs(pgl_host_read, pgl_host_write);
}

/* ========== Filesystem population ==========
*
* The standalone build uses an in-memory filesystem (WASMFS); the host fills it
* (share/ data, PGDATA) before starting Postgres.
* Both return 0 on success and -errno on failure.
*/
int EMSCRIPTEN_KEEPALIVE pgl_fs_mkdir(const char *path, int mode) {
    if (mkdir(path, mode) == 0 || errno == EEXIST)
        return 0;
    return -errno;
}

int EMSCRIPTEN_KEEPALIVE pgl_fs_write_file(const char *path, const void *buf, size_t len, int mode) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0)
        return -errno;
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            int err = errno;
            close(fd);
            return -err;
        }
        buf = (const char *) buf + n;
        len -= n;
    }
    close(fd);
    return 0;
}

/* like mkdir -p, for the parent directories of path */
static int pgl_fs_mkdir_parents(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        int rc = pgl_fs_mkdir(path, 0700);
        *p = '/';
        if (rc != 0)
            return rc;
    }
    return 0;
}

static size_t pgl_tar_octal(const char *field, size_t len) {
    size_t value = 0;
    for (size_t i = 0; i < len && field[i]; i++) {
        if (field[i] >= '0' && field[i] <= '7')
            value = value * 8 + (field[i] - '0');
    }
    return value;
}

/*
* Unpack an (uncompressed) tarball into the filesystem under prefix, creating
* parent directories as needed. Supports regular files and directories, with
* ustar name prefixes, GNU long names and pax path records. Doing this inside
* the module saves the host a call per file.
* Returns 0 on success and -errno on failure.
*/
int EMSCRIPTEN_KEEPALIVE pgl_fs_load_tar(const char *prefix, const char *tar, size_t len) {
    char path[PATH_MAX];
    char long_name[PATH_MAX] = "";
    size_t offset = 0;

    while (offset + 512 <= len) {
        const char *header = tar + offset;
        const char *data = header + 512;
        char name[256];
        size_t size;
        char type;
        int mode;
        int rc;

        if (header[0] == '\0')
            break;          /* end of archive */
        size = pgl_tar_octal(header + 124, 12);
        type = header[156] ? header[156] : '0';
        mode = (int) pgl_tar_octal(header + 100, 8) & 0777;
        if (offset + 512 + size > len)
            return -EINVAL;
        offset += 512 + (size + 511) / 512 * 512;

        if (type == 'L') {              /* GNU long name for the next entry */
            snprintf(long_name, sizeof(long_name), "%.*s", (int) size, data);
            continue;
        }
        if (type == 'x') {              /* pax extended header */
            const char *rec = data;
            while (rec < data + size) {
                size_t rec_len = strtoul(rec, NULL, 10);
                const char *kv = memchr(rec, ' ', size - (rec - data));
                if (rec_len == 0 || kv == NULL)
                    break;
                if (strncmp(kv + 1, "path=", 5) == 0)
                    snprintf(long_name, sizeof(long_name), "%.*s",
                             (int) (rec + rec_len - (kv + 6) - 1), kv + 6);
                rec += rec_len;
            }
            continue;
        }
        if (type == 'g')                /* pax global header */
            continue;

        if (long_name[0]) {
            snprintf(name, sizeof(name), "%s", long_name);
            long_name[0] = '\0';
        } else if (memcmp(header + 257, "ustar", 5) == 0 && header[345]) {
            snprintf(name, sizeof(name), "%.155s/%.100s", header + 345, header);
        } else {
            snprintf(name, sizeof(name), "%.100s", header);
        }

        /* strip leading "./" and "/", and trailing "/" */
        const char *rel = name;
        while (rel[0] == '/' || (rel[0] == '.' && rel[1] == '/'))
            rel += rel[0] == '/' ? 1 : 2;
        size_t rel_len = strlen(rel);
        while (rel_len > 0 && rel[rel_len - 1] == '/')
            rel_len--;
        if (rel_len == 0 || (rel_len == 1 && rel[0] == '.'))
            continue;
        if (snprintf(path, sizeof(path), "%s/%.*s",
                     strcmp(prefix, "/") == 0 ? "" : prefix, (int) rel_len, rel) >= (int) sizeof(path))
            return -ENAMETOOLONG;

        rc = pgl_fs_mkdir_parents(path);
        if (rc != 0)
            return rc;
        if (type == '5') {
            rc = pgl_fs_mkdir(path, mode ? mode : 0700);
        } else if (type == '0' || type == '7') {
            rc = pgl_fs_write_file(path, data, size, mode ? mode : 0600);
        } else {
            return -ENOTSUP;
        }
        if (rc != 0)
            return rc;
    }
    return 0;
}

/* ========== Filesystem export ==========
*
* The counterparts of pgl_fs_load_tar(), for the host to get files out of the
* module, e.g. an initialized PGDATA. The result is malloc'd and the host
* frees it. All return 0 on success and -errno on failure.
*/
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} pgl_buf;

static int pgl_buf_reserve(pgl_buf *b, size_t extra) {
    if (b->len + extra <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 65536;
    while (cap < b->len + extra)
        cap *= 2;
    char *data = realloc(b->data, cap);
    if (data == NULL)
        return -ENOMEM;
    b->data = data;
    b->cap = cap;
    return 0;
}

/* append size bytes from fd, plus zero padding to a 512 byte block */
static int pgl_buf_append_file(pgl_buf *b, int fd, size_t size) {
    size_t padded = (size + 511) / 512 * 512;
    size_t done = 0;
    int rc = pgl_buf_reserve(b, padded);
    if (rc != 0)
        return rc;
    while (done < size) {
        ssize_t n = read(fd, b->data + b->len + done, size - done);
        if (n < 0)
            return -errno;
        if (n == 0)
            return -EIO;    /* the file shrank */
        done += n;
    }
    memset(b->data + b->len + size, 0, padded - size);
    b->len += padded;
    return 0;
}

static int pgl_tar_header(pgl_buf *b, const char *name, char type, int mode, size_t size) {
    size_t name_len = strlen(name);
    unsigned int sum = 0;
    char *h;
    int rc;

    if (name_len > 100) {
        /* GNU long name: the name is the data of a preceding 'L' entry */
        rc = pgl_tar_header(b, "././@LongLink", 'L', 0, name_len + 1);
        if (rc != 0 || (rc = pgl_buf_reserve(b, (name_len + 1 + 511) / 512 * 512)) != 0)
            return rc;
        memset(b->data + b->len, 0, (name_len + 1 + 511) / 512 * 512);
        memcpy(b->data + b->len, name, name_len);
        b->len += (name_len + 1 + 511) / 512 * 512;
    }
    rc = pgl_buf_reserve(b, 512);
    if (rc != 0)
        return rc;
    h = b->data + b->len;
    memset(h, 0, 512);
    memcpy(h, name, name_len > 100 ? 100 : name_len);
    snprintf(h + 100, 8, "%07o", mode & 07777);
    snprintf(h + 108, 8, "%07o", 0);            /* uid */
    snprintf(h + 116, 8, "%07o", 0);            /* gid */
    snprintf(h + 124, 12, "%011zo", size);
    snprintf(h + 136, 12, "%011o", 0);          /* mtime */
    memset(h + 148, ' ', 8);                    /* checksum, counted as spaces */
    h[156] = type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    for (int i = 0; i < 512; i++)
        sum += (unsigned char) h[i];
    snprintf(h + 148, 7, "%06o", sum);
    h[155] = ' ';
    b->len += 512;
    return 0;
}

/* add the contents of directory path to the archive, named rel + entry name */
static int pgl_tar_add_dir(pgl_buf *b, const char *path, const char *rel) {
    char child[PATH_MAX];
    char child_rel[PATH_MAX];
    struct dirent *de;
    struct stat st;
    int rc = 0;
    DIR *dir = opendir(path);

    if (dir == NULL)
        return -errno;
    while (rc == 0 && (de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int) sizeof(child) ||
            snprintf(child_rel, sizeof(child_rel), "%s%s", rel, de->d_name) >= (int) sizeof(child_rel) - 1) {
            rc = -ENAMETOOLONG;
            break;
        }
        if (lstat(child, &st) != 0) {
            rc = -errno;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            strcat(child_rel, "/");
            rc = pgl_tar_header(b, child_rel, '5', st.st_mode, 0);
            if (rc == 0)
                rc = pgl_tar_add_dir(b, child, child_rel);
        } else if (S_ISREG(st.st_mode)) {
            int fd = open(child, O_RDONLY);
            if (fd < 0) {
                rc = -errno;
                break;
            }
            rc = pgl_tar_header(b, child_rel, '0', st.st_mode, st.st_size);
            if (rc == 0)
                rc = pgl_buf_append_file(b, fd, st.st_size);
            close(fd);
        }
        /* other file types (symlinks, devices) are skipped */
    }
    closedir(dir);
    return rc;
}

/*
* Pack the contents of directory path into an (uncompressed) tarball, with
* names relative to path. pgl_fs_load_tar(path, ...) restores it.
*/
int EMSCRIPTEN_KEEPALIVE pgl_fs_dump_tar(const char *path, char **tar, size_t *len) {
    pgl_buf b = {0};
    int rc = pgl_tar_add_dir(&b, path, "");

    if (rc == 0 && (rc = pgl_buf_reserve(&b, 1024)) == 0) {
        memset(b.data + b.len, 0, 1024);        /* end of archive */
        b.len += 1024;
    }
    if (rc != 0) {
        free(b.data);
        return rc;
    }
    *tar = b.data;
    *len = b.len;
    return 0;
}

int EMSCRIPTEN_KEEPALIVE pgl_fs_read_file(const char *path, char **data, size_t *len) {
    pgl_buf b = {0};
    struct stat st;
    int rc;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -errno;
    if (fstat(fd, &st) != 0) {
        rc = -errno;
    } else {
        rc = pgl_buf_append_file(&b, fd, st.st_size);
    }
    close(fd);
    if (rc != 0) {
        free(b.data);
        return rc;
    }
    *data = b.data;
    *len = st.st_size;
    return 0;
}

/* like rm -rf: a path that does not exist is not an error */
int EMSCRIPTEN_KEEPALIVE pgl_fs_remove_tree(const char *path) {
    char child[PATH_MAX];
    struct dirent *de;
    struct stat st;
    int rc = 0;
    DIR *dir;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -errno;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0 ? 0 : -errno;
    dir = opendir(path);
    if (dir == NULL)
        return -errno;
    while (rc == 0 && (de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int) sizeof(child))
            rc = -ENAMETOOLONG;
        else
            rc = pgl_fs_remove_tree(child);
    }
    closedir(dir);
    if (rc == 0 && rmdir(path) != 0)
        rc = -errno;
    return rc;
}

/* ========== Subprocesses ==========
*
* initdb runs postgres as a subprocess, with system() and popen(). As in the
* regular build (see initdb.ts in the parent repository), the host runs it in
* a separate instance of the postgres module, and a pipe is a file.
*
* The host import pglite.exec(command, stdin_path, stdout_path) runs command
* with its stdin and stdout redirected from and to files in this instance's
* filesystem (each may be NULL), keeping PGDATA in sync between the two
* instances, and returns its exit code, or -1 if it cannot run the command.
*/
PGL_HOST_IMPORT(exec) extern int pgl_host_exec(const char *command, const char *stdin_path, const char *stdout_path);

#define PGL_PIPE_TO_CHILD "/tmp/pglite-pipe-to-child"
#define PGL_PIPE_FROM_CHILD "/tmp/pglite-pipe-from-child"

static FILE *pgl_pipe = NULL;
static char *pgl_pipe_command = NULL;   /* popen(..., "w"): runs at pclose() */
static int pgl_pipe_exit_code = 0;      /* popen(..., "r"): has run already */

/* exit code to wait status; a command that cannot run exits with 127, like in a shell */
static int pgl_wait_status(int exit_code) {
    return (exit_code < 0 ? 127 : exit_code & 0xff) << 8;
}

static ssize_t pgl_host_system(const char *command) {
    return pgl_wait_status(pgl_host_exec(command, NULL, NULL));
}

static FILE *pgl_host_popen(const char *command, const char *mode) {
    if (pgl_pipe != NULL) {
        errno = EMFILE;     /* one pipe at a time */
        return NULL;
    }
    if (mode[0] == 'r') {
        pgl_pipe_exit_code = pgl_host_exec(command, NULL, PGL_PIPE_FROM_CHILD);
        if (pgl_pipe_exit_code < 0) {
            errno = ENOENT;
            return NULL;
        }
        pgl_pipe = fopen(PGL_PIPE_FROM_CHILD, "r");
    } else if (mode[0] == 'w') {
        pgl_pipe_command = strdup(command);
        if (pgl_pipe_command == NULL)
            return NULL;
        pgl_pipe = fopen(PGL_PIPE_TO_CHILD, "w");
        if (pgl_pipe == NULL) {
            free(pgl_pipe_command);
            pgl_pipe_command = NULL;
        }
    } else {
        errno = EINVAL;
        return NULL;
    }
    return pgl_pipe;
}

static int pgl_host_pclose(FILE *stream) {
    int exit_code;

    if (stream == NULL || stream != pgl_pipe) {
        errno = ECHILD;
        return -1;
    }
    fclose(stream);
    pgl_pipe = NULL;
    if (pgl_pipe_command != NULL) {
        exit_code = pgl_host_exec(pgl_pipe_command, PGL_PIPE_TO_CHILD, NULL);
        free(pgl_pipe_command);
        pgl_pipe_command = NULL;
        unlink(PGL_PIPE_TO_CHILD);
    } else {
        exit_code = pgl_pipe_exit_code;
        unlink(PGL_PIPE_FROM_CHILD);
    }
    return pgl_wait_status(exit_code);
}

typedef ssize_t (*pgl_system_t)(const char *command);
typedef FILE *(*pgl_popen_t)(const char *command, const char *mode);
typedef int (*pgl_pclose_t)(FILE *stream);
extern void pgl_set_system_fn(pgl_system_t system_fn);
extern void pgl_set_popen_fn(pgl_popen_t popen_fn);
extern void pgl_set_pclose_fn(pgl_pclose_t pclose_fn);

__attribute__((constructor))
static void pgl_standalone_init_subprocesses(void) {
    pgl_set_system_fn(pgl_host_system);
    pgl_set_popen_fn(pgl_host_popen);
    pgl_set_pclose_fn(pgl_host_pclose);
}

/* ========== Extensions ==========
*
* There is no dynamic loading: extensions are linked into the module (see
* pglitec_standalone.h), and dlopen() finds them by file name. The file must
* still exist, as dfmgr.c checks that first.
*/

/* the default for programs without extensions (initdb) */
__attribute__((weak)) const pgl_static_library pgl_static_libraries[] = {{NULL, NULL}};

/* libc's dlerror() reports these */
extern void __dl_seterr(const char *fmt, ...);

void *dlopen(const char *filename, int flags) {
    const char *base = strrchr(filename, '/');
    size_t len;

    base = base ? base + 1 : filename;
    len = strcspn(base, ".");
    for (const pgl_static_library *lib = pgl_static_libraries; lib->name; lib++) {
        if (strlen(lib->name) == len && strncmp(lib->name, base, len) == 0)
            return (void *) lib;
    }
    __dl_seterr("%s: not linked into the standalone module", filename);
    return NULL;
}

/* a NULL handle (RTLD_DEFAULT) searches all libraries */
void *dlsym(void *restrict handle, const char *restrict name) {
    for (const pgl_static_library *lib = handle ? handle : pgl_static_libraries; lib->name; lib++) {
        for (const pgl_static_symbol *sym = lib->symbols; sym->name; sym++) {
            if (strcmp(sym->name, name) == 0)
                return sym->address;
        }
        if (handle)
            break;
    }
    __dl_seterr("undefined symbol: %s", name);
    return NULL;
}

int dlclose(void *handle) {
    return 0;
}

/* ========== libc functions emscripten implements in JS ========== */

/*
* There are no signals in the standalone build. Accept and ignore timer
* requests: timeout-based features such as statement_timeout and lock_timeout
* do not fire.
*/
int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value) {
    if (old_value)
        memset(old_value, 0, sizeof(*old_value));
    return 0;
}

/* only basebackup.c calls getegid(); like pgl_getuid(), pretend to be root */
int __syscall_getegid32(void) {
    return 0;
}

/*
* There is no network, so only numeric addresses are resolved (enough for
* pg_hba.conf parsing).
*/
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    struct {
        struct addrinfo ai;
        union {
            struct sockaddr_in in4;
            struct sockaddr_in6 in6;
        } sa;
    } *r;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int port = service ? atoi(service) : 0;

    if (node == NULL)
        node = (hints && (hints->ai_flags & AI_PASSIVE)) ? "0.0.0.0" : "127.0.0.1";

    r = calloc(1, sizeof(*r));
    if (r == NULL)
        return EAI_MEMORY;

    if ((family == AF_UNSPEC || family == AF_INET) &&
        inet_pton(AF_INET, node, &r->sa.in4.sin_addr) == 1) {
        r->sa.in4.sin_family = AF_INET;
        r->sa.in4.sin_port = htons(port);
        r->ai.ai_family = AF_INET;
        r->ai.ai_addrlen = sizeof(struct sockaddr_in);
    } else if ((family == AF_UNSPEC || family == AF_INET6) &&
               inet_pton(AF_INET6, node, &r->sa.in6.sin6_addr) == 1) {
        r->sa.in6.sin6_family = AF_INET6;
        r->sa.in6.sin6_port = htons(port);
        r->ai.ai_family = AF_INET6;
        r->ai.ai_addrlen = sizeof(struct sockaddr_in6);
    } else {
        free(r);
        return EAI_NONAME;
    }
    r->ai.ai_socktype = hints ? hints->ai_socktype : 0;
    r->ai.ai_protocol = hints ? hints->ai_protocol : 0;
    r->ai.ai_addr = (struct sockaddr *) &r->sa;
    *res = &r->ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    /* getaddrinfo above returns a single allocation holding one entry */
    free(res);
}

int getnameinfo(const struct sockaddr *addr, socklen_t addrlen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    return EAI_FAIL;
}

#endif /* PGLITE_STANDALONE */
