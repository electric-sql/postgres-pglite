#include <unistd.h>
#include <stdio.h>
#include <sys/shm.h>
#include <errno.h>
#include <time.h>
#include <pwd.h>
#include <sys/types.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
// TODO: an include for libpglite
#endif

typedef ssize_t (*pglite_system_t)(const char *command);
pglite_system_t pglite_system = NULL;

void EMSCRIPTEN_KEEPALIVE
pgl_set_system_fn(pglite_system_t system_fn) {
    pglite_system = system_fn;
}

int EMSCRIPTEN_KEEPALIVE
pgl_system(const char *command) {
    if (pglite_system) {
        return pglite_system(command);
    }
    return 123; // should we call system???
}

typedef FILE* (*pglite_popen_t)(const char *command, const char *mode);
pglite_popen_t pglite_popen = NULL;

void EMSCRIPTEN_KEEPALIVE
pgl_set_popen_fn(pglite_popen_t popen_fn) {
    pglite_popen = popen_fn;
}

FILE* EMSCRIPTEN_KEEPALIVE
pgl_popen(const char *command, const char *mode) {
    if (pglite_popen) {
        return pglite_popen(command, mode);
    }
    return popen(command, mode);
}

typedef int (*pglite_pclose_t)(FILE* stream);
pglite_pclose_t pglite_pclose = NULL;

void EMSCRIPTEN_KEEPALIVE
pgl_set_pclose_fn(pglite_pclose_t pclose_fn) {
    pglite_pclose = pclose_fn;
}

int EMSCRIPTEN_KEEPALIVE
pgl_pclose(FILE* stream) {
    if (pglite_pclose) {
        return pglite_pclose(stream);
    }
    return pclose(stream);
}


#define PGLITE_UID 123

uid_t EMSCRIPTEN_KEEPALIVE
pgl_geteuid(void) {
    return PGLITE_UID;
}

uid_t EMSCRIPTEN_KEEPALIVE
pgl_getuid(void) {
    return PGLITE_UID;
}

struct passwd* EMSCRIPTEN_KEEPALIVE
pgl_getpwuid(uid_t uid) {
    static struct passwd pw;
    static char name[] = "web_user";
    static char passwd[] = "x";
    static char gecos[] = "Static User";
    static char dir[] = "/home/web_user";
    static char shell[] = "/bin/sh";

    pw.pw_name   = name;
    pw.pw_passwd = passwd;
    pw.pw_uid    = uid;
    pw.pw_gid    = uid;
    pw.pw_gecos  = gecos;
    pw.pw_dir    = dir;
    pw.pw_shell  = shell;

    return &pw;
}


FILE* pgl_stdin = NULL;
FILE* pgl_stdout = NULL;

void EMSCRIPTEN_KEEPALIVE
pgl_exit(int status) {
    if (pgl_stdin != NULL) {
        fclose(pgl_stdin);
        pgl_stdin = NULL;
    }
    if (pgl_stdout != NULL) {
        fclose(pgl_stdout);
        pgl_stdout = NULL;
    }
    optind = 1;
    exit(status);
}

FILE * EMSCRIPTEN_KEEPALIVE
pgl_freopen(const char *pathname, const char *mode, int streamid) {
    if (streamid == 0) {
        pgl_stdin = freopen(pathname, mode, stdin);
        return pgl_stdin;
    }
    if (streamid == 1) {
        pgl_stdout = freopen(pathname, mode, stdout);
        return pgl_stdout;
    }
    if (streamid == 2) {
        return freopen(pathname, mode, stderr);
    }
    return NULL;
}

// ============ SHM ===============

typedef struct ShmSegment {
    int shmid;
    key_t key;
    size_t size;
    void *addr;
    int shmflg;
    struct ShmSegment *next;
} ShmSegment;

static ShmSegment *shm_list = NULL;
static unsigned int next_shmid = 1;

// shmget replacement
int EMSCRIPTEN_KEEPALIVE
shmget(key_t key, size_t size, int shmflg) {
    ShmSegment *seg = shm_list;

    // Search for existing segment
    while (seg) {
        if (seg->key == key) return seg->shmid;
        seg = seg->next;
    }

    // If IPC_CREAT is set, create new segment
    if (shmflg & IPC_CREAT) {
        int pagesize = getpagesize();
        while (pagesize < size) pagesize += pagesize;
        void *mem = malloc(pagesize);
        if (!mem) {
            errno = ENOMEM;
            return -1;
        }

        ShmSegment *new_seg = malloc(sizeof(ShmSegment));
        if (!new_seg) {
            free(mem);
            errno = ENOMEM;
            return -1;
        }

        new_seg->shmid = next_shmid++;
        new_seg->key = key;
        new_seg->size = size;
        new_seg->addr = mem;
        new_seg->shmflg = shmflg;
        new_seg->next = shm_list;
        shm_list = new_seg;

        return new_seg->shmid;
    }

    errno = ENOENT;
    return -1;
}

// shmat replacement
void EMSCRIPTEN_KEEPALIVE
*shmat(int shmid, const void *shmaddr, int shmflg) {
    ShmSegment *seg = shm_list;

    while (seg) {
        if (seg->shmid == shmid) {
            return seg->addr;
        }
        seg = seg->next;
    }

    errno = EINVAL;
    return (void *)-1;
}

// shmdt replacement
int EMSCRIPTEN_KEEPALIVE
shmdt(const void *shmaddr) {
    ShmSegment *seg = shm_list;

    while (seg) {
        if (seg->addr == shmaddr) {
            return 0;
        }
        seg = seg->next;
    }

    errno = EINVAL;
    return -1;
}

// shmctl replacement
int EMSCRIPTEN_KEEPALIVE
shmctl(int shmid, int cmd, struct shmid_ds *buf) {
    ShmSegment *seg = shm_list;
    ShmSegment *prev = NULL;

    while (seg) {
        if (seg->shmid == shmid) {
            if (cmd == IPC_RMID) {
                free(seg->addr);
                if (prev) prev->next = seg->next;
                else shm_list = seg->next;
                free(seg);
                return 0;
            } else if (cmd == IPC_STAT && buf != NULL) {
                buf->shm_segsz = seg->size;
                buf->shm_perm.__key = seg->key;
                buf->shm_nattch = 0;
                buf->shm_atime = buf->shm_dtime = buf->shm_ctime = time(NULL);
                return 0;
            } else if (cmd == IPC_SET && buf != NULL) {
                seg->size = buf->shm_segsz;
                return 0;
            } else {
                fprintf(stderr, "pglitec: shmctl: no such cmd %d\n", cmd);
                errno = EINVAL;
                return -1;
            }
        }
        prev = seg;
        seg = seg->next;
    }

    fprintf(stderr, "pglitec: shmctl: no such segment %d\n", shmid);
    errno = EINVAL;
    return -1;
}

// ========== MMAP/MUNMAP ==========

/*
 * Dummy munmap implementation for emscripten.
 * Emscripten's munmap can corrupt unrelated files in MEMFS,
 * so we just return success without doing anything.
 * Memory will be reclaimed when the WASM instance terminates.
 */
int EMSCRIPTEN_KEEPALIVE
munmap(void *addr, size_t length) {
    (void)addr;
    (void)length;
    return 0;
}