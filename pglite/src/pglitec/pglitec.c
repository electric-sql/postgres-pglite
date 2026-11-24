/*-------------------------------------------------------------------------
 *
 * pglitec.c
 *	  PGlite libc overrides
 *
 *
 *
 * NOTES
 *    this file contains "libc" function wrappers for PGlite, as well as 
 *    other flags used by PostgreSQL and needed by PGlite. These are 
 *    needed in order to emulate some system calls related to sockets, 
 *    user management etc.
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <stdio.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <setjmp.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/wait.h>
#include <fcntl.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
// TODO: an include for libpglite
#endif

volatile int is_pglite_active = 0;

void EMSCRIPTEN_KEEPALIVE clear_setitimer(void) {
    struct itimerval zero = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &zero, NULL);
}

int pgl_setPGliteActive(int newValue) {
	int current = is_pglite_active;
	is_pglite_active = newValue;
    if (newValue == 0) {
        clear_setitimer();
    }
	return current;
}

/* ========== Top level exception handling ==========
*
* In Postgres, the top level sigsetjmp handles exceptions encountered during executions
* In PGlite, we handle the top level sigsetjmp manually by exiting on the corresponding longjmp 
* with a predefined exit code (POSTGRES_MAIN_LONGJMP). We only need to override the longjmp 
* because setjmp already behaves as expected.
* This keeps the code changes cleaner.
*/

#define POSTGRES_MAIN_LONGJMP 100

volatile sigjmp_buf	postgresmain_sigjmp_buf;

volatile bool ignore_till_sync = false;
volatile bool send_ready_for_query = false;

/*
* This wraps the libc longjmp() to enable us to intercept and handle the main longjmp manually
*/
void EMSCRIPTEN_KEEPALIVE pgl_longjmp(jmp_buf env, int val) {
    if (is_pglite_active && memcmp(env, (void*)postgresmain_sigjmp_buf, sizeof(jmp_buf)) == 0) {
        // reset this as it is expected
        if (!ignore_till_sync)
		    send_ready_for_query = true;	/* initially, or after error */
        exit(POSTGRES_MAIN_LONGJMP);
    }
    longjmp(env, val);
}

// emscripten defines siglongjmp as longjmp
void EMSCRIPTEN_KEEPALIVE pgl_siglongjmp(sigjmp_buf env, int val) {
    pgl_longjmp(env, val);
}

/* ========== Process handling functions ==========
*
* We wrap some process handling functions to emulate 
* the behavior of an OS when instantiating a new process.
* This is not available in emscripten atm, so we handle it manually
* See pglite.ts in the frontend on how we emulate this instantiation.
*/
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
    // if pglite_system is not set, we assume we cannot exec that command and return != 0
    // we could also just call system() and let it crash, but this leads to some stderr messages on Windows
    return 123;
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

/* ========== User related functions ==========
*
* PostgreSQL code expects the current user to fulfill certain criteria to be allowed to run the process, otherwise it exits. 
* This is irrelevant in WASM/emscripten, so we fake the expected data to match what Postgres wants.
*/

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
    static char name[] = "postgres";
    static char passwd[] = "x";
    static char gecos[] = "Static User";
    static char dir[] = "/home/postgres";
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

/* ========== atexit functions ==========
*
* atexit registered functions provide important functionality in Postgres
* we need to be able to run them when closing PGlite.
*/
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

void EMSCRIPTEN_KEEPALIVE pgl_run_atexit_funcs(void) {
    // Call in reverse registration order
    for (int i = atexit_func_count - 1; i >= 0; --i) {
        if (atexit_funcs[i]) {
            atexit_funcs[i]();
        }
    }
    atexit_func_count = 0;
}

/* ========== streams functions ==========
*
* initdb communicates with postgres via stdin<->stdout redirection
* we need to handle this manually mainly because we're also handling processes manually
*/

FILE* pgl_stdin = NULL;
FILE* pgl_stdout = NULL;

/*
* we override exit() to make sure we cleanup the stdin/stdout file descriptors
*/
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
    exit(status);
}

/*
* Overrides freopen() libc function to allow initdb<->PGlite comm via standard streams (see initdb.ts in frontend)
*/
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
    int backing_fd;
    struct ShmSegment *next;
} ShmSegment;

static ShmSegment *shm_list = NULL;
static unsigned int next_shmid = 1;

/*
* System V segments are owned by the OS, not by a process: a segment created
* by the postmaster must be found, by key, from every other process
* (PGSharedMemoryReAttach in an EXEC_BACKEND child).
* When the frontend installs the functions below, the segment table lives
* there (see OS in processUtils.ts) and is common to all processes.
* Without them we fall back to a table private to this process.
*
* The frontend functions return a negative PGL_SHM_E* code on failure.
*/
#define PGL_SHM_ENOENT (-1)
#define PGL_SHM_EEXIST (-2)
#define PGL_SHM_EINVAL (-3)
#define PGL_SHM_ENOMEM (-4)

/* what shmctl(IPC_STAT) needs to know about a segment */
typedef struct {
    unsigned int size;
    int key;
    unsigned int nattch;
    int cpid;
} PglShmStat;

typedef int (*pglite_shmget_t)(key_t key, size_t size, int shmflg);
typedef int (*pglite_shmat_t)(int shmid, const void *shmaddr, int shmflg);
typedef int (*pglite_shmdt_t)(const void *shmaddr);
typedef int (*pglite_shmctl_t)(int shmid, int cmd, PglShmStat *stat);

pglite_shmget_t pglite_shmget = NULL;
pglite_shmat_t pglite_shmat = NULL;
pglite_shmdt_t pglite_shmdt = NULL;
pglite_shmctl_t pglite_shmctl = NULL;

pglite_shmget_t EMSCRIPTEN_KEEPALIVE pgl_set_shmget_fn(pglite_shmget_t shmget_fn) {
    pglite_shmget_t prev = pglite_shmget;
    pglite_shmget = shmget_fn;
    return prev;
}

pglite_shmat_t EMSCRIPTEN_KEEPALIVE pgl_set_shmat_fn(pglite_shmat_t shmat_fn) {
    pglite_shmat_t prev = pglite_shmat;
    pglite_shmat = shmat_fn;
    return prev;
}

pglite_shmdt_t EMSCRIPTEN_KEEPALIVE pgl_set_shmdt_fn(pglite_shmdt_t shmdt_fn) {
    pglite_shmdt_t prev = pglite_shmdt;
    pglite_shmdt = shmdt_fn;
    return prev;
}

pglite_shmctl_t EMSCRIPTEN_KEEPALIVE pgl_set_shmctl_fn(pglite_shmctl_t shmctl_fn) {
    pglite_shmctl_t prev = pglite_shmctl;
    pglite_shmctl = shmctl_fn;
    return prev;
}

/* translate a PGL_SHM_E* code into errno; always returns -1 */
static int pgl_shm_error(int code) {
    switch (code) {
        case PGL_SHM_ENOENT: errno = ENOENT; break;
        case PGL_SHM_EEXIST: errno = EEXIST; break;
        case PGL_SHM_ENOMEM: errno = ENOMEM; break;
        default: errno = EINVAL; break;
    }
    return -1;
}

// shmget replacement
int EMSCRIPTEN_KEEPALIVE
pgl_shmget(key_t key, size_t size, int shmflg) {
    if (pglite_shmget) {
        int shmid = pglite_shmget(key, size, shmflg);
        return shmid < 0 ? pgl_shm_error(shmid) : shmid;
    }

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

        mkdir("/pglite_shm", 0700);
        char path[64];
        snprintf(path, sizeof(path), "/pglite_shm/sysv_%d", (int)key);
        int bfd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (bfd >= 0) {
            write(bfd, mem, pagesize);
        }
        new_seg->backing_fd = bfd;

        shm_list = new_seg;

        return new_seg->shmid;
    }

    errno = ENOENT;
    return -1;
}

// shmat replacement
void EMSCRIPTEN_KEEPALIVE
*pgl_shmat(int shmid, const void *shmaddr, int shmflg) {
    if (pglite_shmat) {
        int addr = pglite_shmat(shmid, shmaddr, shmflg);
        if (addr < 0) {
            pgl_shm_error(addr);
            return (void *)-1;
        }
        return (void *)addr;
    }

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
    if (pglite_shmdt) {
        int ret = pglite_shmdt(shmaddr);
        return ret < 0 ? pgl_shm_error(ret) : 0;
    }

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
    if (pglite_shmctl) {
        PglShmStat st = {0, 0, 0, 0};
        int ret = pglite_shmctl(shmid, cmd, &st);
        if (ret < 0)
            return pgl_shm_error(ret);
        if (cmd == IPC_STAT && buf != NULL) {
            memset(buf, 0, sizeof(struct shmid_ds));
            buf->shm_segsz = st.size;
            buf->shm_perm.__key = st.key;
            buf->shm_nattch = st.nattch;
            buf->shm_cpid = st.cpid;
            buf->shm_atime = buf->shm_dtime = buf->shm_ctime = time(NULL);
        }
        return 0;
    }

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

// ============ POSIX SHM ===============

int EMSCRIPTEN_KEEPALIVE
pgl_shm_open(const char *name, int oflag, mode_t mode) {
    // char path[256];
    // snprintf(path, sizeof(path), "/tmp/pglite_shm%s", name);
    return open(name, oflag, mode);
}

int EMSCRIPTEN_KEEPALIVE
pgl_shm_unlink(const char *name) {
    // char path[256];
    // snprintf(path, sizeof(path), "/tmp/pglite_shm%s", name);
    return unlink(name);
}

// ============ MMAP/MUNMAP ===============

typedef struct MmapRegion {
    void *addr;
    size_t length;
    int backing_fd;
    struct MmapRegion *next;
} MmapRegion;

static MmapRegion *mmap_list = NULL;

typedef void (*hlp_shmem_t)(unsigned int, unsigned int);
hlp_shmem_t hlp_shmem = NULL;

hlp_shmem_t EMSCRIPTEN_KEEPALIVE set_hlp_shmem(hlp_shmem_t new) {
    hlp_shmem_t prev = hlp_shmem;
    hlp_shmem = new;
    return prev;
}

/*
* A MAP_SHARED mapping of a file must show the same memory to every process
* mapping that file (this is how Postgres' dynamic shared memory segments
* work). The frontend keeps the table of such mappings, see OS in
* processUtils.ts.
*
* pglite_mmap returns the address, 0 when it does not handle the request
* (we then fall back to a private copy), or a negative PGL_SHM_E* code.
* pglite_munmap returns 0 when it released a mapping, non-zero when the
* address is not one of its mappings.
*/
typedef int (*pglite_mmap_t)(size_t length, int fd, int offset);
typedef int (*pglite_munmap_t)(void *addr, size_t length);

pglite_mmap_t pglite_mmap = NULL;
pglite_munmap_t pglite_munmap = NULL;

pglite_mmap_t EMSCRIPTEN_KEEPALIVE pgl_set_mmap_fn(pglite_mmap_t mmap_fn) {
    pglite_mmap_t prev = pglite_mmap;
    pglite_mmap = mmap_fn;
    return prev;
}

pglite_munmap_t EMSCRIPTEN_KEEPALIVE pgl_set_munmap_fn(pglite_munmap_t munmap_fn) {
    pglite_munmap_t prev = pglite_munmap;
    pglite_munmap = munmap_fn;
    return prev;
}

void * EMSCRIPTEN_KEEPALIVE
pgl_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)prot;

    if (pglite_mmap && fd >= 0 && (flags & MAP_SHARED)) {
        int shared = pglite_mmap(length, fd, (int) offset);
        if (shared < 0) {
            pgl_shm_error(shared);
            return MAP_FAILED;
        }
        if (shared > 0)
            return (void *) shared;
    }

    void *mem = malloc(length);
    if (!mem) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    memset(mem, 0, length);

    if (fd >= 0) {
        off_t saved = lseek(fd, 0, SEEK_CUR);
        lseek(fd, offset, SEEK_SET);
        read(fd, mem, length);
        lseek(fd, saved, SEEK_SET);
    }

    MmapRegion *r = malloc(sizeof(MmapRegion));
    if (!r) {
        free(mem);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    r->addr = mem;
    r->length = length;

    // int myfd = 0;
    // if (fd >= 0) {
    //     myfd = dup(fd);
    // } else {
    //     myfd = open("/tmp/.shmem", O_RDWR | O_CREAT, 0600);
    // }

    r->backing_fd = (fd >= 0) ? dup(fd) : -1;
    r->next = mmap_list;
    mmap_list = r;

    // int myfd = open("/tmp/.shmem", O_RDWR | O_CREAT | O_TRUNC, 0600);
    // dprintf(myfd, "%lu\n%lu\n", r->addr, r->length);
    // close(myfd);

    if (hlp_shmem && r->backing_fd == -1) {
        hlp_shmem((unsigned int) r->addr, (unsigned int) r->length);
    }

    return mem;
}

int EMSCRIPTEN_KEEPALIVE
pgl_munmap(void *addr, size_t length) {
    if (pglite_munmap && pglite_munmap(addr, length) == 0)
        return 0;
    MmapRegion *r = mmap_list, *prev = NULL;
    while (r) {
        if (r->addr == addr) {
            // first, flush the data to the backing FS
            write(r->backing_fd, r->addr, r->length);
            // safe to close the backing_fd
            close(r->backing_fd);
            if (prev) prev->next = r->next;
            else mmap_list = r->next;
            free(r->addr);
            free(r);
            return 0;
        }
        prev = r;
        r = r->next;
    }
    return 0;
}

// ============ SHM FLUSH/LOAD ===============

void EMSCRIPTEN_KEEPALIVE pgl_shm_flush(void) {
    for (ShmSegment *s = shm_list; s; s = s->next) {
        if (s->backing_fd >= 0) {
            lseek(s->backing_fd, 0, SEEK_SET);
            write(s->backing_fd, s->addr, s->size);
        }
    }
    for (MmapRegion *r = mmap_list; r; r = r->next) {
        if (r->backing_fd >= 0) {
            lseek(r->backing_fd, 0, SEEK_SET);
            write(r->backing_fd, r->addr, r->length);
        }
    }
}

void EMSCRIPTEN_KEEPALIVE pgl_shm_load(void) {
    for (ShmSegment *s = shm_list; s; s = s->next) {
        if (s->backing_fd >= 0) {
            lseek(s->backing_fd, 0, SEEK_SET);
            read(s->backing_fd, s->addr, s->size);
        }
    }
    for (MmapRegion *r = mmap_list; r; r = r->next) {
        if (r->backing_fd >= 0) {
            lseek(r->backing_fd, 0, SEEK_SET);
            read(r->backing_fd, r->addr, r->length);
        }
    }
}

/* ============ SOCKET EMULATION =============
*
* To exchange data between the backend (Postgres) and frontend (JS part of PGlite),
* we emulate a socket by overriding the following libc functions.
*/

/* 
* read FROM JS
* Callback used for reading data from the frontend
*/
typedef ssize_t (*pglite_recv_t)(int __fd, void *__buf, size_t __n, int __flags);
pglite_recv_t pglite_recv = NULL;

pglite_recv_t EMSCRIPTEN_KEEPALIVE pgl_set_recv_fn(pglite_recv_t recv_fn) {
    pglite_recv_t prev = pglite_recv;
    pglite_recv = recv_fn;
    return prev;
}

/* write TO JS
* Callback used for writing data to the frontend
*/
typedef ssize_t (*pglite_send_t)(int __fd, const void *__buf, size_t __n, int __flags);
pglite_send_t pglite_send = NULL;

pglite_send_t EMSCRIPTEN_KEEPALIVE pgl_set_send_fn(pglite_send_t send_fn) {
    pglite_send_t prev = pglite_send;
    pglite_send = send_fn;
    return prev;
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

/*
 * Mirror of ClientSocket from libpq-be.h:
 *   typedef struct ClientSocket { pgsocket sock; SockAddr raddr; } ClientSocket;
 * where pgsocket = int, SockAddr = { sockaddr_storage addr; socklen_t salen; }
 */
typedef struct {
	int			sock;
	struct sockaddr_storage addr;
	socklen_t	salen;
} PglClientSocket;

static PglClientSocket pgl_dummy_client_socket;
static int pgl_dummy_client_socket_inited = 0;

static void pgl_init_dummy_client_socket(void) {
	struct sockaddr_un *un;
	memset(&pgl_dummy_client_socket, 0, sizeof(pgl_dummy_client_socket));
	pgl_dummy_client_socket.sock = open("/tmp/.s.PGSQL.5432.connected", O_RDWR | O_CREAT, 0600);
	un = (struct sockaddr_un *)&pgl_dummy_client_socket.addr;
	un->sun_family = AF_UNIX;
	strcpy(un->sun_path, "/tmp/.s.PGSQL.5432.connected");
	pgl_dummy_client_socket.salen = sizeof(struct sockaddr_un);
}

void *EMSCRIPTEN_KEEPALIVE hlp_get_dummy_client_socket_ptr(void) {
	if (!pgl_dummy_client_socket_inited) {
		pgl_init_dummy_client_socket();
		pgl_dummy_client_socket_inited = 1;
	}
	return &pgl_dummy_client_socket;
}

// static int next_socket_fd = 0;

// typedef int (*pglite_socket_t)(int domain, int type, int protocol);
// pglite_socket_t pglite_socket = NULL;

// pglite_socket_t EMSCRIPTEN_KEEPALIVE pgl_set_socket_fn(pglite_socket_t socket_fn) {
//     pglite_socket_t prev = pglite_socket;
//     pglite_socket = socket_fn;
//     return prev;
// }

// int EMSCRIPTEN_KEEPALIVE pgl_socket(int domain, int type, int protocol) {
//     if (pglite_socket) {
//         return pglite_socket(domain, type, protocol);
//     }
//     return next_socket_fd++;
// }

// typedef int (*pglite_bind_t)(int socket, const struct sockaddr *address, socklen_t address_len);
// pglite_bind_t pglite_bind = NULL;

// pglite_bind_t EMSCRIPTEN_KEEPALIVE pgl_set_bind_fn(pglite_bind_t bind_fn) {
//     pglite_bind_t prev = pglite_bind;
//     pglite_bind = bind_fn;
//     return prev;
// }

// int EMSCRIPTEN_KEEPALIVE pgl_bind(int socket, const struct sockaddr *address, socklen_t address_len) {
//     if (pglite_bind) {
//         return pglite_bind(socket, address, address_len);
//     }
// 	return 0;
// }

// typedef int (*pglite_listen_t)(int socket, int backlog);
// pglite_listen_t pglite_listen = NULL;

// pglite_listen_t EMSCRIPTEN_KEEPALIVE pgl_set_listen_fn(pglite_listen_t listen_fn) {
//     pglite_listen_t prev = pglite_listen;
//     pglite_listen = listen_fn;
//     return prev;
// }

// int EMSCRIPTEN_KEEPALIVE pgl_listen(int socket, int backlog) {
//     if (pglite_listen) {
//         return pglite_listen(socket, backlog);
//     }
// 	return 0;
// }

// typedef int (*pglite_accept_t)(int socket, struct sockaddr *address, socklen_t *address_len);
// pglite_accept_t pglite_accept = NULL;

// pglite_accept_t EMSCRIPTEN_KEEPALIVE pgl_set_accept_fn(pglite_accept_t accept_fn) {
//     pglite_accept_t prev = pglite_accept;
//     pglite_accept = accept_fn;
//     return prev;
// }

// int EMSCRIPTEN_KEEPALIVE pgl_accept(int socket, struct sockaddr *address, socklen_t *address_len) {
//     if (pglite_accept) {
//         return pglite_accept(socket, address, address_len);
//     }
//     return next_socket_fd++;
// }

typedef int (*pglite_close_t)(int fd);
pglite_close_t pglite_close = NULL;

pglite_close_t EMSCRIPTEN_KEEPALIVE pgl_set_close_fn(pglite_close_t close_fn) {
    pglite_close_t prev = pglite_close;
    pglite_close = close_fn;
    return prev;
}

int EMSCRIPTEN_KEEPALIVE pgl_close(int fd) {
    if (pglite_close) {
        int ret = pglite_close(fd);
        if (ret < 0) {
            return close(fd);
        }
        return ret;
    }
    return close(fd);
}

/*
* Overrides the recv() libc function
*/

ssize_t EMSCRIPTEN_KEEPALIVE pgl_recv(int __fd, void *__buf, size_t __n, int __flags) {
	ssize_t got = pglite_recv(__fd, __buf, __n, __flags);
	return got;
}

/*
* Overrides the send() libc function
*/

ssize_t EMSCRIPTEN_KEEPALIVE pgl_send(int __fd, const void *__buf, size_t __n, int __flags) {
	ssize_t wrote = pglite_send(__fd, __buf, __n, __flags);
	return wrote;
}

int EMSCRIPTEN_KEEPALIVE pgl_connect(int socket, const struct sockaddr *address, socklen_t address_len) {
	// dummy
	return 0;
}

typedef struct Latch
{
	sig_atomic_t is_set;
	sig_atomic_t maybe_sleeping;
	bool		is_shared;
	int			owner_pid;
} Latch;

__attribute__((weak)) struct Latch *MyLatch = NULL;

typedef ssize_t (*pglite_poll_t)(struct pollfd fds[], ssize_t nfds, int timeout);
pglite_poll_t pglite_poll = NULL;

pglite_poll_t EMSCRIPTEN_KEEPALIVE pgl_set_poll_fn(pglite_poll_t poll_fn) {
    pglite_poll_t prev = pglite_poll;
    pglite_poll = poll_fn;
    return prev;
}

int EMSCRIPTEN_KEEPALIVE pgl_poll(struct pollfd fds[], ssize_t nfds, int timeout) {
    // if (pglite_poll) {
    //     return pglite_poll(fds, nfds, timeout);
    // }
    int ret = poll(fds, nfds, 0);
    if (ret == 0 && (!MyLatch || !MyLatch->is_set))
        exit(102);
    return ret;
}

/*
* Emulate process creation for multi-process
*
* Postgres is built with EXEC_BACKEND, so the postmaster starts a child by
* writing its parameters to a file and then doing fork() + execv() of
* "postgres --forkchild=<kind> <param file>".
*
* Our fork() never returns 0: it asks the frontend to create a new Postgres
* instance that will run main() with the arguments above, and returns the
* new pid to the postmaster. execv() is therefore never reached.
*
* fork() has no arguments, so the launch parameters are recorded by
* __wrap_postmaster_child_launch (see pglitec_launch.c) right before
* Postgres calls fork().
*/

int pglite_child_type = 0; // BackendType
const char *pglite_child_kind = NULL; // name used in --forkchild=<kind>
char pglite_child_param_file[1024] = ""; // file holding the BackendParameters
size_t pglite_child_startup_data_len = 0; // length of the startup data at the end of that file

/*
* The parameter file is a BackendParameters struct followed by the startup
* data. The postmaster writes offsetof(BackendParameters, startup_data) bytes
* of the struct, but the child reads sizeof(BackendParameters). The two are
* equal on 64-bit platforms. On wasm32 the struct is 8-byte aligned and ends
* with a 4-byte size_t, so sizeof can be 4 bytes larger: the child then gets
* a short read, or startup data shifted by the padding.
* Make the file match what the child reads by inserting the padding between
* the struct and the startup data.
*/
#define PGL_BACKEND_PARAMETERS_ALIGN 8

static void pgl_pad_param_file(void) {
    struct stat st;
    if (stat(pglite_child_param_file, &st) != 0)
        return;
    if ((size_t) st.st_size < pglite_child_startup_data_len)
        return;
    size_t struct_len = (size_t) st.st_size - pglite_child_startup_data_len;
    size_t pad = (PGL_BACKEND_PARAMETERS_ALIGN - struct_len % PGL_BACKEND_PARAMETERS_ALIGN) % PGL_BACKEND_PARAMETERS_ALIGN;
    if (pad == 0)
        return;

    char *buf = malloc(st.st_size + pad);
    if (!buf)
        return;
    FILE *fp = fopen(pglite_child_param_file, "rb");
    if (!fp || fread(buf, 1, struct_len, fp) != struct_len) {
        if (fp) fclose(fp);
        free(buf);
        return;
    }
    memset(buf + struct_len, 0, pad);
    size_t got = fread(buf + struct_len + pad, 1, pglite_child_startup_data_len, fp);
    fclose(fp);
    if (got == pglite_child_startup_data_len) {
        fp = fopen(pglite_child_param_file, "wb");
        if (fp) {
            fwrite(buf, 1, st.st_size + pad, fp);
            fclose(fp);
        }
    }
    free(buf);
}

typedef pid_t (*pglite_fork_t)(int child_type, const char *child_kind, const char *param_file);
pglite_fork_t pglite_fork = NULL;

pglite_fork_t EMSCRIPTEN_KEEPALIVE pgl_set_fork_fn(pglite_fork_t fork_fn) {
    pglite_fork_t prev = pglite_fork;
    pglite_fork = fork_fn;
    return prev;
}

pid_t EMSCRIPTEN_KEEPALIVE pgl_fork() {
    if (pglite_fork) {
        if (!pglite_child_kind) {
            // fork() called outside of postmaster_child_launch: we would not
            // know what to start
            errno = ENOSYS;
            return -1;
        }
        pgl_pad_param_file();
        return pglite_fork(pglite_child_type, pglite_child_kind, pglite_child_param_file);
    }
    return fork();
}

/*
* Emulate kill()
*/

typedef int (*pglite_kill_t)(pid_t pid, int signal);
pglite_kill_t pglite_kill = NULL;

pglite_kill_t EMSCRIPTEN_KEEPALIVE pgl_set_kill_fn(pglite_kill_t kill_fn) {
    pglite_kill_t prev = pglite_kill;
    pglite_kill = kill_fn;
    return prev;
}

pid_t EMSCRIPTEN_KEEPALIVE pgl_kill(pid_t pid, int signal) {
    if (pglite_kill) {
        int ret = pglite_kill(pid, signal);
        if (ret < 0) {
            // the only failure is an unknown pid; Postgres relies on errno
            // to tell a dead process from one it may not signal
            errno = ESRCH;
        }
        return ret;
    }
    return kill(pid, signal);
}

/*
* Emulate sigaction()
* Extracts sa_handler from the struct, delegates to JS, and populates oldact.
*/

typedef void (*pgl_sighandler_t)(int);
typedef pgl_sighandler_t (*pglite_sigaction_t)(int signum, pgl_sighandler_t handler);
pglite_sigaction_t pglite_sigaction = NULL;

pglite_sigaction_t EMSCRIPTEN_KEEPALIVE pgl_set_sigaction_fn(pglite_sigaction_t sigaction_fn) {
    pglite_sigaction_t prev = pglite_sigaction;
    pglite_sigaction = sigaction_fn;
    return prev;
}

int EMSCRIPTEN_KEEPALIVE pgl_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    if (pglite_sigaction) {
        pgl_sighandler_t new_handler = act ? act->sa_handler : SIG_DFL;
        pgl_sighandler_t prev = pglite_sigaction(signum, new_handler);
        if (oldact) {
            memset(oldact, 0, sizeof(struct sigaction));
            oldact->sa_handler = prev;
        }
        return 0;
    }
    return sigaction(signum, act, oldact);
}

/*
* Emulate waitpid()
*/

typedef pid_t (*pglite_waitpid_t)(pid_t pid, int *status, int options);
pglite_waitpid_t pglite_waitpid = NULL;

pglite_waitpid_t EMSCRIPTEN_KEEPALIVE pgl_set_waitpid_fn(pglite_waitpid_t waitpid_fn) {
    pglite_waitpid_t prev = pglite_waitpid;
    pglite_waitpid = waitpid_fn;
    return prev;
}

pid_t EMSCRIPTEN_KEEPALIVE pgl_waitpid(pid_t pid, int *status, int options) {
    if (pglite_waitpid) {
        return pglite_waitpid(pid, status, options);
    }
    return waitpid(pid, status, options);
}

/**
 * Emulate getpid()
 */

typedef int (*pglite_getpid_t)();
pglite_getpid_t pglite_getpid = NULL;

pglite_getpid_t EMSCRIPTEN_KEEPALIVE pgl_set_getpid_fn(pglite_getpid_t getpid_fn) {
    pglite_getpid_t prev = pglite_getpid;
    pglite_getpid = getpid_fn;
    return prev;
}

pid_t EMSCRIPTEN_KEEPALIVE pgl_getpid() {
    if (pglite_getpid) {
        return pglite_getpid();
    }
    return getpid();
}




// extern int postmaster_alive_fds[2];

// void EMSCRIPTEN_KEEPALIVE pgl_set_postmaster_alive_fds(int fd0, int fd1) {
//     postmaster_alive_fds[0] = fd0;
//     postmaster_alive_fds[1] = fd1;
// }