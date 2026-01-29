#include <unistd.h>
#include <stdio.h>
#include <sys/shm.h>
#include <errno.h>
#include <time.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <setjmp.h>
#include <string.h>

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

#define MAX_ATEXIT_FUNCS 32

static void (*atexit_funcs[MAX_ATEXIT_FUNCS])(void);
static int atexit_func_count = 0;

int EMSCRIPTEN_KEEPALIVE pgl_atexit(void (*function)(void)) {
    if (atexit_func_count >= MAX_ATEXIT_FUNCS) {
        // According to the C standard, atexit returns nonzero on failure.
        return -1;
    }
    atexit_funcs[atexit_func_count++] = function;
    return 0;
}

static void pgl_run_atexit_funcs(void) {
    // Call in reverse registration order
    for (int i = atexit_func_count - 1; i >= 0; --i) {
        if (atexit_funcs[i]) {
            atexit_funcs[i]();
        }
    }
    atexit_func_count = 0;
}

void EMSCRIPTEN_KEEPALIVE
pgl_exit(int status) {
    if (pgl_stdin != NULL) {
        fclose(pgl_stdin);
        pgl_stdin = NULL;
    }
    if (pgl_stdout != NULL) {
        fflush(pgl_stdout);
        fclose(pgl_stdout);
        pgl_stdout = NULL;
    }
    optind = 1;
    if (status == 99) {
        // status 99 is when we don't want to actually exit, so we DON'T run atexits
        // exit 0 to make the calling process happy
        exit(0);
    } else {
        // this is not a pglite exit, so run atexits
        pgl_run_atexit_funcs();
    }
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
pgl_shmget(key_t key, size_t size, int shmflg) {
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
*pgl_shmat(int shmid, const void *shmaddr, int shmflg) {
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
pgl_shmdt(const void *shmaddr) {
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
pgl_shmctl(int shmid, int cmd, struct shmid_ds *buf) {
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
pgl_munmap(void *addr, size_t length) {
    (void)addr;
    (void)length;
    // dummy
    return 0;
}

// read FROM JS
// (i guess return number of bytes written)
// ssize_t pgl_read(/* ignored */ int socket, void *buffer, size_t length,/* ignored */ int flags,/* ignored */ void *address,/* ignored */ socklen_t *address_len);
//typedef ssize_t (*pgl_read_t)(/* ignored */ int socket, void *buffer, size_t length,/* ignored */ int flags,/* ignored */ void *address,/* ignored */ unsigned int *address_len);
typedef ssize_t (*pgl_read_t)(void *buffer, size_t max_length);
pgl_read_t pgl_read;

// write TO JS
// (i guess return number of bytes read)
// ssize_t pgl_write(/* ignored */ int sockfd, const void *buf, size_t len, /* ignored */ int flags);
// typedef ssize_t (*pgl_write_t)(/* ignored */ int sockfd, const void *buf, size_t len, /* ignored */ int flags);
typedef ssize_t (*pgl_write_t)(void *buffer, size_t length);
pgl_write_t pgl_write;

void EMSCRIPTEN_KEEPALIVE
pgl_set_rw_cbs(pgl_read_t read_cb, pgl_write_t write_cb) {
    pgl_read = read_cb;
    pgl_write = write_cb;
}

int EMSCRIPTEN_KEEPALIVE pgl_fcntl(int __fd, int __cmd, ...) {
	// dummy 
	return 0;
}

int EMSCRIPTEN_KEEPALIVE pgl_setsockopt(int __fd, int __level, int __optname,
	const void *__optval, socklen_t __optlen) {
	// dummy 
	return 0;
}

int EMSCRIPTEN_KEEPALIVE pgl_getsockopt(int __fd, int __level, int __optname,
	void *__restrict __optval,
	socklen_t *__restrict __optlen) {
	// dummy 
	return 0;
}

int EMSCRIPTEN_KEEPALIVE pgl_getsockname(int __fd, struct sockaddr * __addr,
	socklen_t *__restrict __len) {
	// dummy 
	return 0;
}

ssize_t EMSCRIPTEN_KEEPALIVE pgl_recv(int __fd, void *__buf, size_t __n, int __flags) {
	ssize_t got = pgl_read(__buf, __n);
	return got;
}

ssize_t EMSCRIPTEN_KEEPALIVE pgl_send(int __fd, const void *__buf, size_t __n, int __flags) {
	ssize_t wrote = pgl_write(__buf, __n);
	return wrote;
}

int EMSCRIPTEN_KEEPALIVE pgl_connect(int socket, const struct sockaddr *address, socklen_t address_len) {
	// dummy
	return 0;
}

struct pollfd {
    int   fd;         /* file descriptor */
    short events;     /* requested events */
	short revents;    /* returned events */
};

int EMSCRIPTEN_KEEPALIVE pgl_poll(struct pollfd fds[], ssize_t nfds, int timeout) {
    // dummy
	return nfds;
}

#define POSTGRES_MAIN_LONGJMP 100

volatile sigjmp_buf	postgresmain_sigjmp_buf;
volatile int is_pglite_active = 0;
// extern bool ExitOnAnyError;
// extern BackendType MyBackendType;

int pgl_setPGliteActive(int newValue) {
	int current = is_pglite_active;
	is_pglite_active = newValue;
	return current;
}

void EMSCRIPTEN_KEEPALIVE pgl_longjmp(jmp_buf env, int val) {
    if (is_pglite_active && memcmp(env, (void*)postgresmain_sigjmp_buf, sizeof(jmp_buf)) == 0) {
        exit(POSTGRES_MAIN_LONGJMP);
    }
    longjmp(env, val);
}

// emscripten defines siglongjmp as longjmp
void EMSCRIPTEN_KEEPALIVE pgl_siglongjmp(sigjmp_buf env, int val) {
    pgl_longjmp(env, val);
}