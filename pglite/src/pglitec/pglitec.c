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
#include <errno.h>
#include <time.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <setjmp.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include "pglitec.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
/*  TODO: an include for libpglite */
#endif

volatile int is_pglite_active = 0;

/*
 * EXEC_BACKEND normally verifies the child executable by running `postgres
 * -V` through a pipe.  A PGlite process Worker starts another instance of the
 * same statically linked Wasm module, so there is no host subprocess to run.
 * Resolve the preloaded VFS entry while keeping that policy in the PGlite libc
 * boundary instead of teaching PostgreSQL about the JavaScript runtime.
 */
int
pgl_find_other_exec(const char *argv0, const char *target,
					const char *version_string, char *result,
					size_t result_size)
{
	const char *separator;
	size_t		directory_length;
	int			written;

	(void) version_string;
	if (argv0 == NULL || target == NULL || result == NULL ||
		result_size == 0 || strcmp(target, "postgres") != 0)
	{
		errno = EINVAL;
		return -1;
	}
	separator = strrchr(argv0, '/');
	directory_length = separator == NULL ? 0 : (size_t) (separator - argv0);
	written = snprintf(result, result_size, "%.*s%s%s",
					   (int) directory_length, argv0,
					   separator == NULL ? "" : "/", target);
	if (written < 0 || (size_t) written >= result_size ||
		access(result, X_OK) != 0)
		return -1;
	return 0;
}

/*
 * Emscripten's generated WASI file helpers close over memory 0.  PostgreSQL
 * legitimately performs storage I/O directly into shared buffers, so the
 * postmaster profile bounces only tagged ranges through a private temporary.
 * This keeps the generated host ABI private-only without changing PostgreSQL
 * buffer-management code.
 */
static int
pgl_is_tagged_range(const void *pointer)
{
	return (((uintptr_t) pointer) & UINT32_C(0xc0000000)) != 0;
}

static ssize_t
pgl_bounced_read(int fd, void *buffer, size_t length, off_t offset,
				 int positioned)
{
	void	   *temporary;
	ssize_t		result;
	int			saved_errno;

	if (!pgl_is_tagged_range(buffer) || length == 0)
		return positioned ? pread(fd, buffer, length, offset) :
			read(fd, buffer, length);
	temporary = malloc(length);
	if (temporary == NULL)
	{
		errno = ENOMEM;
		return -1;
	}
	result = positioned ? pread(fd, temporary, length, offset) :
		read(fd, temporary, length);
	saved_errno = errno;
	if (result > 0)
		memcpy(buffer, temporary, (size_t) result);
	free(temporary);
	errno = saved_errno;
	return result;
}

static ssize_t
pgl_bounced_write(int fd, const void *buffer, size_t length, off_t offset,
				  int positioned)
{
	void	   *temporary;
	ssize_t		result;
	int			saved_errno;

	if (!pgl_is_tagged_range(buffer) || length == 0)
		return positioned ? pwrite(fd, buffer, length, offset) :
			write(fd, buffer, length);
	temporary = malloc(length);
	if (temporary == NULL)
	{
		errno = ENOMEM;
		return -1;
	}
	memcpy(temporary, buffer, length);
	result = positioned ? pwrite(fd, temporary, length, offset) :
		write(fd, temporary, length);
	saved_errno = errno;
	free(temporary);
	errno = saved_errno;
	return result;
}

ssize_t		EMSCRIPTEN_KEEPALIVE
pgl_fd_read(int fd, void *buffer, size_t length)
{
	return pgl_bounced_read(fd, buffer, length, 0, 0);
}

ssize_t		EMSCRIPTEN_KEEPALIVE
pgl_fd_write(int fd, const void *buffer, size_t length)
{
	return pgl_bounced_write(fd, buffer, length, 0, 0);
}

ssize_t		EMSCRIPTEN_KEEPALIVE
pgl_fd_pread(int fd, void *buffer, size_t length, off_t offset)
{
	return pgl_bounced_read(fd, buffer, length, offset, 1);
}

ssize_t		EMSCRIPTEN_KEEPALIVE
pgl_fd_pwrite(int fd, const void *buffer, size_t length, off_t offset)
{
	return pgl_bounced_write(fd, buffer, length, offset, 1);
}

static ssize_t
pgl_bounced_iov(int fd, const struct iovec *iov, int iovcnt, off_t offset,
				int positioned, int writing)
{
	ssize_t		total = 0;
	int			index;

	if (iovcnt < 0)
	{
		errno = EINVAL;
		return -1;
	}
	for (index = 0; index < iovcnt; index++)
	{
		ssize_t		part;

		if (writing)
			part = pgl_bounced_write(fd, iov[index].iov_base,
									 iov[index].iov_len, offset,
									 positioned);
		else
			part = pgl_bounced_read(fd, iov[index].iov_base,
									iov[index].iov_len, offset,
									positioned);
		if (part < 0)
			return total > 0 ? total : -1;
		total += part;
		if (positioned)
			offset += part;
		if ((size_t) part != iov[index].iov_len)
			break;
	}
	return total;
}

ssize_t		EMSCRIPTEN_KEEPALIVE
pgl_fd_readv(int fd, const struct iovec *iov, int iovcnt)
{
	return pgl_bounced_iov(fd, iov, iovcnt, 0, 0, 0);
}

ssize_t		EMSCRIPTEN_KEEPALIVE
pgl_fd_writev(int fd, const struct iovec *iov, int iovcnt)
{
	return pgl_bounced_iov(fd, iov, iovcnt, 0, 0, 1);
}

ssize_t		EMSCRIPTEN_KEEPALIVE
pgl_fd_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	return pgl_bounced_iov(fd, iov, iovcnt, offset, 1, 0);
}

ssize_t		EMSCRIPTEN_KEEPALIVE
pgl_fd_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	return pgl_bounced_iov(fd, iov, iovcnt, offset, 1, 1);
}

/*
 * Explicit private-pointer identity for the multi-memory transformer. Keep
 * this out of line so the marker survives ordinary per-translation-unit LLVM
 * optimization. The post-linker removes release calls after consuming their
 * provenance; debug transforms retain this check.
 */
void	   *EMSCRIPTEN_KEEPALIVE
__attribute__((noinline))
pgl_private_pointer(void *pointer)
{
	if ((intptr_t) pointer <= 0)
	{
		__builtin_trap();
	}
	return pointer;
}

void		EMSCRIPTEN_KEEPALIVE
clear_setitimer(void)
{
	struct itimerval zero = {{0, 0}, {0, 0}};

	setitimer(ITIMER_REAL, &zero, NULL);
}

int
pgl_setPGliteActive(int newValue)
{
	int			current = is_pglite_active;

	is_pglite_active = newValue;
	if (newValue == 0)
	{
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

volatile sigjmp_buf postgresmain_sigjmp_buf;

volatile bool ignore_till_sync = false;
volatile bool send_ready_for_query = false;

/*
* This wraps the libc longjmp() to enable us to intercept and handle the main longjmp manually
*/
void		EMSCRIPTEN_KEEPALIVE
pgl_longjmp(jmp_buf env, int val)
{
	if (is_pglite_active && memcmp(env, (void *) postgresmain_sigjmp_buf, sizeof(jmp_buf)) == 0)
	{
		/* reset this as it is expected */
		if (!ignore_till_sync)
			send_ready_for_query = true;	/* initially, or after error */
		exit(POSTGRES_MAIN_LONGJMP);
	}
	longjmp(env, val);
}

/*  emscripten defines siglongjmp as longjmp */
void		EMSCRIPTEN_KEEPALIVE
pgl_siglongjmp(sigjmp_buf env, int val)
{
	pgl_longjmp(env, val);
}

/* ========== Process handling functions ==========
*
* We wrap some process handling functions to emulate
* the behavior of an OS when instantiating a new process.
* This is not available in emscripten atm, so we handle it manually
* See pglite.ts in the frontend on how we emulate this instantiation.
*/
typedef ssize_t (*pglite_system_t) (const char *command);
pglite_system_t pglite_system = NULL;

void		EMSCRIPTEN_KEEPALIVE
pgl_set_system_fn(pglite_system_t system_fn)
{
	pglite_system = system_fn;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_system(const char *command)
{
	if (pglite_system)
	{
		return pglite_system(command);
	}

	/*
	 * if pglite_system is not set, we assume we cannot exec that command and
	 * return != 0
	 */

	/*
	 * we could also just call system() and let it crash, but this leads to
	 * some stderr messages on Windows
	 */
	return 123;
}

typedef FILE *(*pglite_popen_t) (const char *command, const char *mode);
pglite_popen_t pglite_popen = NULL;

void		EMSCRIPTEN_KEEPALIVE
pgl_set_popen_fn(pglite_popen_t popen_fn)
{
	pglite_popen = popen_fn;
}

FILE	   *EMSCRIPTEN_KEEPALIVE
pgl_popen(const char *command, const char *mode)
{
	if (pglite_popen)
	{
		return pglite_popen(command, mode);
	}
	return popen(command, mode);
}

typedef int (*pglite_pclose_t) (FILE *stream);
pglite_pclose_t pglite_pclose = NULL;

void		EMSCRIPTEN_KEEPALIVE
pgl_set_pclose_fn(pglite_pclose_t pclose_fn)
{
	pglite_pclose = pclose_fn;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_pclose(FILE *stream)
{
	if (pglite_pclose)
	{
		return pglite_pclose(stream);
	}
	return pclose(stream);
}

/* ========== Postmaster process host ========== */

typedef pid_t (*pglite_spawn_backend_t) (const char *, const char *, int);
typedef pid_t (*pglite_getpid_t) (void);
typedef int (*pglite_kill_t) (pid_t, int);
typedef pid_t (*pglite_waitpid_t) (pid_t, int *, int);
typedef int (*pglite_timer_t) (double, double);
typedef uint32_t (*pglite_signal_poll_t) (void);
typedef void (*pglite_signal_mask_t) (uint32_t);
typedef int (*pglite_futex_wait_t) (void *, uint32_t, double);
typedef int (*pglite_futex_wake_t) (void *, int);
typedef int (*pglite_socket_t) (int, int, int);
typedef int (*pglite_bind_t) (int, const struct sockaddr *, socklen_t);
typedef int (*pglite_listen_t) (int, int);
typedef int (*pglite_accept_t) (int, struct sockaddr *, socklen_t *);
typedef int (*pglite_close_t) (int);
typedef ssize_t (*pglite_recv_t) (int, void *, size_t, int);
typedef ssize_t (*pglite_send_t) (int, const void *, size_t, int);
typedef int (*pglite_poll_t) (struct pollfd *, nfds_t, int);

static pglite_spawn_backend_t pglite_spawn_backend = NULL;
static pglite_getpid_t pglite_getpid = NULL;
static pglite_kill_t pglite_kill = NULL;
static pglite_waitpid_t pglite_waitpid = NULL;
static pglite_timer_t pglite_timer = NULL;
static pglite_signal_poll_t pglite_signal_poll = NULL;
static pglite_signal_mask_t pglite_signal_mask = NULL;
static pglite_futex_wait_t pglite_futex_wait = NULL;
static pglite_futex_wake_t pglite_futex_wake = NULL;
static pglite_socket_t pglite_socket = NULL;
static pglite_bind_t pglite_bind = NULL;
static pglite_listen_t pglite_listen = NULL;
static pglite_accept_t pglite_accept = NULL;
static pglite_close_t pglite_close = NULL;
static pglite_recv_t pglite_recv = NULL;
static pglite_send_t pglite_send = NULL;
static pglite_poll_t pglite_poll = NULL;
static struct sigaction pglite_signal_actions[NSIG];
static sigset_t pglite_blocked_signals;

void		EMSCRIPTEN_KEEPALIVE
pgl_set_process_host(pglite_spawn_backend_t spawn_backend,
					 pglite_getpid_t get_process_id,
					 pglite_kill_t send_signal,
					 pglite_waitpid_t wait_process)
{
	pglite_spawn_backend = spawn_backend;
	pglite_getpid = get_process_id;
	pglite_kill = send_signal;
	pglite_waitpid = wait_process;
}

void		EMSCRIPTEN_KEEPALIVE
pgl_set_signal_host(pglite_signal_poll_t poll_signals,
					pglite_signal_mask_t set_signal_mask,
					pglite_timer_t set_timer)
{
	pglite_signal_poll = poll_signals;
	pglite_signal_mask = set_signal_mask;
	pglite_timer = set_timer;
}

void		EMSCRIPTEN_KEEPALIVE
pgl_set_futex_host(pglite_futex_wait_t wait_futex,
				   pglite_futex_wake_t wake_futex)
{
	pglite_futex_wait = wait_futex;
	pglite_futex_wake = wake_futex;
}

void		EMSCRIPTEN_KEEPALIVE
pgl_set_socket_host(pglite_socket_t create_socket,
					pglite_bind_t bind_socket,
					pglite_listen_t listen_socket,
					pglite_accept_t accept_socket,
					pglite_close_t close_socket,
					pglite_recv_t receive_socket,
					pglite_send_t send_socket,
					pglite_poll_t poll_sockets)
{
	pglite_socket = create_socket;
	pglite_bind = bind_socket;
	pglite_listen = listen_socket;
	pglite_accept = accept_socket;
	pglite_close = close_socket;
	pglite_recv = receive_socket;
	pglite_send = send_socket;
	pglite_poll = poll_sockets;
}

pid_t		EMSCRIPTEN_KEEPALIVE
pgl_spawn_backend(const char *child_kind, const char *parameter_file,
				  int client_socket)
{
	if (pglite_spawn_backend == NULL)
	{
		errno = ENOSYS;
		return -1;
	}
	return pglite_spawn_backend(child_kind, parameter_file, client_socket);
}

pid_t		EMSCRIPTEN_KEEPALIVE
pgl_getpid(void)
{
	if (pglite_getpid == NULL)
		return getpid();
	return pglite_getpid();
}

int			EMSCRIPTEN_KEEPALIVE
pgl_kill(pid_t pid, int signal_number)
{
	if (pglite_kill == NULL)
	{
		errno = ESRCH;
		return -1;
	}
	return pglite_kill(pid, signal_number);
}

pid_t		EMSCRIPTEN_KEEPALIVE
pgl_waitpid(pid_t pid, int *status, int options)
{
	if (pglite_waitpid == NULL)
	{
		errno = ECHILD;
		return -1;
	}
	return pglite_waitpid(pid, status, options);
}

int			EMSCRIPTEN_KEEPALIVE
pgl_setitimer(int which, const struct itimerval *value,
			  struct itimerval *old_value)
{
	double		delay_ms;
	double		interval_ms;

	if (which != ITIMER_REAL || value == NULL || pglite_timer == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	if (old_value != NULL)
		memset(old_value, 0, sizeof(*old_value));
	delay_ms = value->it_value.tv_sec * 1000.0 +
		value->it_value.tv_usec / 1000.0;
	interval_ms = value->it_interval.tv_sec * 1000.0 +
		value->it_interval.tv_usec / 1000.0;
	return pglite_timer(delay_ms, interval_ms);
}

int			EMSCRIPTEN_KEEPALIVE
pgl_sigaction(int signal_number, const struct sigaction *action,
			  struct sigaction *old_action)
{
	if (signal_number <= 0 || signal_number >= NSIG)
	{
		errno = EINVAL;
		return -1;
	}
	if (old_action != NULL)
		*old_action = pglite_signal_actions[signal_number];
	if (action != NULL)
		pglite_signal_actions[signal_number] = *action;
	return 0;
}

static uint32_t
pgl_signal_set_mask(const sigset_t *set)
{
	uint32_t	mask = 0;
	int			signal_number;

	for (signal_number = 1; signal_number <= 31; signal_number++)
	{
		if (sigismember(set, signal_number) == 1)
			mask |= ((uint32_t) 1) << (signal_number - 1);
	}
	return mask;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_sigprocmask(int how, const sigset_t *set, sigset_t *old_set)
{
	int			signal_number;

	if (old_set != NULL)
		*old_set = pglite_blocked_signals;
	if (set == NULL)
		return 0;
	for (signal_number = 1; signal_number < NSIG; signal_number++)
	{
		int			member = sigismember(set, signal_number);

		if (member < 0)
			continue;
		if (how == SIG_SETMASK)
		{
			if (member)
				sigaddset(&pglite_blocked_signals, signal_number);
			else
				sigdelset(&pglite_blocked_signals, signal_number);
		}
		else if (how == SIG_BLOCK && member)
		{
			sigaddset(&pglite_blocked_signals, signal_number);
		}
		else if (how == SIG_UNBLOCK && member)
		{
			sigdelset(&pglite_blocked_signals, signal_number);
		}
	}
	if (how != SIG_SETMASK && how != SIG_BLOCK && how != SIG_UNBLOCK)
	{
		errno = EINVAL;
		return -1;
	}
	if (pglite_signal_mask != NULL)
		pglite_signal_mask(pgl_signal_set_mask(&pglite_blocked_signals));
	return 0;
}

void		EMSCRIPTEN_KEEPALIVE
pgl_dispatch_pending_signals(void)
{
	uint32_t	pending;
	int			signal_number;

	if (pglite_signal_poll == NULL)
		return;
	pending = pglite_signal_poll();
	for (signal_number = 1; signal_number <= 31; signal_number++)
	{
		void		(*handler) (int);

		if ((pending & (((uint32_t) 1) << (signal_number - 1))) == 0)
			continue;
		handler = pglite_signal_actions[signal_number].sa_handler;
		if (handler != SIG_DFL && handler != SIG_IGN && handler != NULL)
			handler(signal_number);
	}
}

int			EMSCRIPTEN_KEEPALIVE
pgl_futex_wait(void *address, uint32_t expected, double timeout_ms)
{
	if (pglite_futex_wait == NULL)
	{
		errno = ENOSYS;
		return -1;
	}
	return pglite_futex_wait(address, expected, timeout_ms);
}

int			EMSCRIPTEN_KEEPALIVE
pgl_futex_wake(void *address, int count)
{
	if (pglite_futex_wake == NULL)
	{
		errno = ENOSYS;
		return -1;
	}
	return pglite_futex_wake(address, count);
}

/* ========== User related functions ==========
*
* PostgreSQL code expects the current user to fulfill certain criteria to be allowed to run the process, otherwise it exits.
* This is irrelevant in WASM/emscripten, so we fake the expected data to match what Postgres wants.
*/

#define PGLITE_UID 123

uid_t		EMSCRIPTEN_KEEPALIVE
pgl_geteuid(void)
{
	return PGLITE_UID;
}

uid_t		EMSCRIPTEN_KEEPALIVE
pgl_getuid(void)
{
	return PGLITE_UID;
}

struct passwd *EMSCRIPTEN_KEEPALIVE
pgl_getpwuid(uid_t uid)
{
	static struct passwd pw;
	static char name[] = "postgres";
	static char passwd[] = "x";
	static char gecos[] = "Static User";
	static char dir[] = "/home/postgres";
	static char shell[] = "/bin/sh";

	pw.pw_name = name;
	pw.pw_passwd = passwd;
	pw.pw_uid = uid;
	pw.pw_gid = uid;
	pw.pw_gecos = gecos;
	pw.pw_dir = dir;
	pw.pw_shell = shell;

	return &pw;
}

/* ========== atexit functions ==========
*
* atexit registered functions provide important functionality in Postgres
* we need to be able to run them when closing PGlite.
*/
#define MAX_ATEXIT_FUNCS 32

static void (*atexit_funcs[MAX_ATEXIT_FUNCS]) (void);
static int	atexit_func_count = 0;

int			EMSCRIPTEN_KEEPALIVE
pgl_atexit(void (*function) (void))
{
	if (atexit_func_count >= MAX_ATEXIT_FUNCS)
	{
		/* According to the C standard, atexit returns nonzero on failure. */
		return -1;
	}
	atexit_funcs[atexit_func_count++] = function;
	return 0;
}

void		EMSCRIPTEN_KEEPALIVE
pgl_run_atexit_funcs(void)
{
	/* Call in reverse registration order */
	for (int i = atexit_func_count - 1; i >= 0; --i)
	{
		if (atexit_funcs[i])
		{
			atexit_funcs[i] ();
		}
	}
	atexit_func_count = 0;
}

/* ========== streams functions ==========
*
* initdb communicates with postgres via stdin<->stdout redirection
* we need to handle this manually mainly because we're also handling processes manually
*/

FILE	   *pgl_stdin = NULL;
FILE	   *pgl_stdout = NULL;

/*
* we override exit() to make sure we cleanup the stdin/stdout file descriptors
*/
void		EMSCRIPTEN_KEEPALIVE
pgl_exit(int status)
{
	if (pgl_stdin != NULL)
	{
		fclose(pgl_stdin);
		pgl_stdin = NULL;
	}
	if (pgl_stdout != NULL)
	{
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
FILE	   *EMSCRIPTEN_KEEPALIVE
pgl_freopen(const char *pathname, const char *mode, int streamid)
{
	if (streamid == 0)
	{
		pgl_stdin = freopen(pathname, mode, stdin);
		return pgl_stdin;
	}
	if (streamid == 1)
	{
		pgl_stdout = freopen(pathname, mode, stdout);
		return pgl_stdout;
	}
	if (streamid == 2)
	{
		return freopen(pathname, mode, stderr);
	}
	return NULL;
}

/*  ============ SHM =============== */

#if defined(__PGLITE_POSTMASTER__) && defined(__PGLITE_MULTI_MEMORY__)

/*
 * The postmaster build has one imported shared Wasm memory for PostgreSQL's
 * cluster-global address space.  Keep the System V namespace and allocator in
 * that memory too, so a fresh EXEC_BACKEND Worker can find and attach a
 * segment without copying process-private C state.
 *
 * The first 128 KiB are reserved for this registry.  Returned pointers carry
 * the 10 tag and are decoded by the multi-memory post-linker.  Allocation is
 * page-aligned, removed blocks are reused, and the host callback grows the
 * imported memory before a new block becomes visible.
 */
#define PGL_SHM_REGISTRY_ADDRESS UINT32_C(0x80010000)
#define PGL_SHM_DATA_OFFSET UINT32_C(0x00020000)
#define PGL_SHM_POINTER_TAG UINT32_C(0x80000000)
#define PGL_SHM_POINTER_MASK UINT32_C(0x3fffffff)
#define PGL_SHM_APERTURE_BYTES UINT32_C(0x40000000)
#define PGL_SHM_PAGE_BYTES UINT32_C(65536)
#define PGL_SHM_MAX_SEGMENTS 256
#define PGL_SHM_MAGIC_INITIALIZING UINT32_C(0x50474c49)
#define PGL_SHM_MAGIC_READY UINT32_C(0x50474c53)
#define PGL_SHM_REGISTRY_VERSION UINT32_C(1)

enum PglShmSlotState
{
	PGL_SHM_SLOT_UNUSED = 0,
	PGL_SHM_SLOT_LIVE = 1,
	PGL_SHM_SLOT_REMOVED = 2,
	PGL_SHM_SLOT_FREE_BLOCK = 3
};

typedef struct PglSharedSegment
{
	uint32_t	state;
	int32_t		shmid;
	int32_t		key;
	uint32_t	requested_size;
	uint32_t	allocated_size;
	uint32_t	offset;
	uint32_t	attach_count;
	int32_t		shmflg;
	int64_t		created_at;
	int64_t		attached_at;
	int64_t		detached_at;
}			PglSharedSegment;

typedef struct PglSharedRegistry
{
	uint32_t	magic;
	uint32_t	version;
	uint32_t	lock;
	uint32_t	next_shmid;
	uint32_t	next_offset;
	uint32_t	allocation_generation;
	PglSharedSegment segments[PGL_SHM_MAX_SEGMENTS];
}			PglSharedRegistry;

typedef int (*pglite_shmem_ensure_capacity_t) (uint32_t required_bytes);
static pglite_shmem_ensure_capacity_t pglite_shmem_ensure_capacity = NULL;

void		EMSCRIPTEN_KEEPALIVE
pgl_set_shmem_host(pglite_shmem_ensure_capacity_t ensure_capacity)
{
	pglite_shmem_ensure_capacity = ensure_capacity;
}

static PglSharedRegistry *
pgl_shm_registry(void)
{
	PglSharedRegistry *registry =
		(PglSharedRegistry *) (uintptr_t) PGL_SHM_REGISTRY_ADDRESS;
	uint32_t	expected = 0;
	uint32_t	magic = __atomic_load_n(&registry->magic, __ATOMIC_ACQUIRE);

	if (magic == PGL_SHM_MAGIC_READY)
		return registry;
	if (magic == 0 &&
		__atomic_compare_exchange_n(&registry->magic, &expected,
									PGL_SHM_MAGIC_INITIALIZING, false,
									__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
	{
		registry->version = PGL_SHM_REGISTRY_VERSION;
		registry->lock = 0;
		registry->next_shmid = 1;
		registry->next_offset = PGL_SHM_DATA_OFFSET;
		registry->allocation_generation = 0;
		__atomic_store_n(&registry->magic, PGL_SHM_MAGIC_READY,
						 __ATOMIC_RELEASE);
		return registry;
	}

	while (__atomic_load_n(&registry->magic, __ATOMIC_ACQUIRE) ==
		   PGL_SHM_MAGIC_INITIALIZING)
		;
	if (__atomic_load_n(&registry->magic, __ATOMIC_ACQUIRE) !=
		PGL_SHM_MAGIC_READY ||
		registry->version != PGL_SHM_REGISTRY_VERSION)
	{
		errno = EPROTO;
		return NULL;
	}
	return registry;
}

static void
pgl_shm_lock(PglSharedRegistry * registry)
{
	uint32_t	expected;

	for (;;)
	{
		expected = 0;
		if (__atomic_compare_exchange_n(&registry->lock, &expected, 1, false,
										__ATOMIC_ACQUIRE,
										__ATOMIC_RELAXED))
			return;
	}
}

static void
pgl_shm_unlock(PglSharedRegistry * registry)
{
	__atomic_store_n(&registry->lock, 0, __ATOMIC_RELEASE);
}

static uint32_t
pgl_shm_align(uint32_t value)
{
	return (value + PGL_SHM_PAGE_BYTES - 1) & ~(PGL_SHM_PAGE_BYTES - 1);
}

static void *
pgl_shm_pointer(uint32_t offset)
{
	return (void *) (uintptr_t) (PGL_SHM_POINTER_TAG | offset);
}

static void
pgl_shm_release_slot(PglSharedRegistry * registry, PglSharedSegment * segment)
{
	segment->state = PGL_SHM_SLOT_FREE_BLOCK;
	segment->shmid = 0;
	segment->key = 0;
	segment->requested_size = 0;
	segment->attach_count = 0;
	segment->shmflg = 0;
	segment->created_at = 0;
	segment->attached_at = 0;
	segment->detached_at = time(NULL);
	registry->allocation_generation++;
}

static PglSharedSegment *
pgl_shm_allocate_slot(PglSharedRegistry * registry, uint32_t requested_size)
{
	uint32_t	allocated_size = pgl_shm_align(requested_size);
	PglSharedSegment *unused = NULL;
	PglSharedSegment *best = NULL;
	unsigned int index;

	if (requested_size == 0 || allocated_size < requested_size)
	{
		errno = EINVAL;
		return NULL;
	}

	for (index = 0; index < PGL_SHM_MAX_SEGMENTS; index++)
	{
		PglSharedSegment *candidate = &registry->segments[index];

		if (candidate->state == PGL_SHM_SLOT_UNUSED && unused == NULL)
			unused = candidate;
		if (candidate->state == PGL_SHM_SLOT_FREE_BLOCK &&
			candidate->allocated_size >= allocated_size &&
			(best == NULL ||
			 candidate->allocated_size < best->allocated_size))
			best = candidate;
	}

	if (best != NULL)
	{
		uint32_t	remainder = best->allocated_size - allocated_size;

		if (remainder >= PGL_SHM_PAGE_BYTES && unused != NULL)
		{
			unused->state = PGL_SHM_SLOT_FREE_BLOCK;
			unused->offset = best->offset + allocated_size;
			unused->allocated_size = remainder;
			best->allocated_size = allocated_size;
		}
		return best;
	}

	if (unused == NULL)
	{
		errno = ENOSPC;
		return NULL;
	}
	if (registry->next_offset > PGL_SHM_APERTURE_BYTES - allocated_size)
	{
		errno = ENOMEM;
		return NULL;
	}
	if (pglite_shmem_ensure_capacity == NULL ||
		pglite_shmem_ensure_capacity(registry->next_offset + allocated_size) !=
		0)
	{
		errno = ENOMEM;
		return NULL;
	}

	unused->offset = registry->next_offset;
	unused->allocated_size = allocated_size;
	registry->next_offset += allocated_size;
	return unused;
}

static PglSharedSegment *
pgl_shm_find_id(PglSharedRegistry * registry, int shmid)
{
	unsigned int index;

	for (index = 0; index < PGL_SHM_MAX_SEGMENTS; index++)
	{
		PglSharedSegment *segment = &registry->segments[index];

		if ((segment->state == PGL_SHM_SLOT_LIVE ||
			 segment->state == PGL_SHM_SLOT_REMOVED) &&
			segment->shmid == shmid)
			return segment;
	}
	return NULL;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_shmget(key_t key, size_t size, int shmflg)
{
	PglSharedRegistry *registry = pgl_shm_registry();
	PglSharedSegment *segment = NULL;
	unsigned int index;
	bool		create = (shmflg & IPC_CREAT) != 0 || key == IPC_PRIVATE;

	if (registry == NULL)
		return -1;
	if (size > UINT32_MAX)
	{
		errno = EINVAL;
		return -1;
	}

	pgl_shm_lock(registry);
	if (key != IPC_PRIVATE)
	{
		for (index = 0; index < PGL_SHM_MAX_SEGMENTS; index++)
		{
			PglSharedSegment *candidate = &registry->segments[index];

			if (candidate->state == PGL_SHM_SLOT_LIVE &&
				candidate->key == (int32_t) key)
			{
				segment = candidate;
				break;
			}
		}
	}

	if (segment != NULL)
	{
		int			result = segment->shmid;

		if (create && (shmflg & IPC_EXCL) != 0)
		{
			errno = EEXIST;
			result = -1;
		}
		else if (size != 0 && size > segment->requested_size)
		{
			errno = EINVAL;
			result = -1;
		}
		pgl_shm_unlock(registry);
		return result;
	}

	if (!create)
	{
		pgl_shm_unlock(registry);
		errno = ENOENT;
		return -1;
	}

	segment = pgl_shm_allocate_slot(registry, (uint32_t) size);
	if (segment == NULL)
	{
		pgl_shm_unlock(registry);
		return -1;
	}

	memset(pgl_shm_pointer(segment->offset), 0, segment->allocated_size);
	segment->state = PGL_SHM_SLOT_LIVE;
	segment->key = (int32_t) key;
	segment->requested_size = (uint32_t) size;
	segment->attach_count = 0;
	segment->shmflg = shmflg;
	segment->created_at = time(NULL);
	segment->attached_at = 0;
	segment->detached_at = 0;
	segment->shmid = (int32_t) registry->next_shmid++;
	if (registry->next_shmid == 0 || registry->next_shmid > INT32_MAX)
		registry->next_shmid = 1;
	registry->allocation_generation++;
	index = (unsigned int) segment->shmid;
	pgl_shm_unlock(registry);
	return (int) index;
}

void		EMSCRIPTEN_KEEPALIVE *
pgl_shmat(int shmid, const void *shmaddr, int shmflg)
{
	PglSharedRegistry *registry = pgl_shm_registry();
	PglSharedSegment *segment;
	void	   *address;

	(void) shmflg;
	if (registry == NULL)
		return (void *) -1;
	pgl_shm_lock(registry);
	segment = pgl_shm_find_id(registry, shmid);
	if (segment == NULL || segment->state != PGL_SHM_SLOT_LIVE)
	{
		pgl_shm_unlock(registry);
		errno = EINVAL;
		return (void *) -1;
	}
	address = pgl_shm_pointer(segment->offset);
	if (shmaddr != NULL && shmaddr != address)
	{
		pgl_shm_unlock(registry);
		errno = EINVAL;
		return (void *) -1;
	}
	segment->attach_count++;
	segment->attached_at = time(NULL);
	pgl_shm_unlock(registry);
	return address;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_shmdt(const void *shmaddr)
{
	PglSharedRegistry *registry = pgl_shm_registry();
	unsigned int index;

	if (registry == NULL)
		return -1;
	pgl_shm_lock(registry);
	for (index = 0; index < PGL_SHM_MAX_SEGMENTS; index++)
	{
		PglSharedSegment *segment = &registry->segments[index];

		if ((segment->state == PGL_SHM_SLOT_LIVE ||
			 segment->state == PGL_SHM_SLOT_REMOVED) &&
			pgl_shm_pointer(segment->offset) == shmaddr)
		{
			if (segment->attach_count == 0)
			{
				pgl_shm_unlock(registry);
				errno = EINVAL;
				return -1;
			}
			segment->attach_count--;
			segment->detached_at = time(NULL);
			if (segment->state == PGL_SHM_SLOT_REMOVED &&
				segment->attach_count == 0)
				pgl_shm_release_slot(registry, segment);
			pgl_shm_unlock(registry);
			return 0;
		}
	}
	pgl_shm_unlock(registry);
	errno = EINVAL;
	return -1;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
	PglSharedRegistry *registry = pgl_shm_registry();
	PglSharedSegment *segment;

	if (registry == NULL)
		return -1;
	pgl_shm_lock(registry);
	segment = pgl_shm_find_id(registry, shmid);
	if (segment == NULL || segment->state != PGL_SHM_SLOT_LIVE)
	{
		pgl_shm_unlock(registry);
		errno = EINVAL;
		return -1;
	}

	if (cmd == IPC_RMID)
	{
		segment->state = PGL_SHM_SLOT_REMOVED;
		segment->key = 0;
		if (segment->attach_count == 0)
			pgl_shm_release_slot(registry, segment);
		pgl_shm_unlock(registry);
		return 0;
	}
	if (cmd == IPC_STAT && buf != NULL)
	{
		memset(buf, 0, sizeof(*buf));
		buf->shm_segsz = segment->requested_size;
		buf->shm_perm.__key = (key_t) segment->key;
		buf->shm_nattch = segment->attach_count;
		buf->shm_ctime = (time_t) segment->created_at;
		buf->shm_atime = (time_t) segment->attached_at;
		buf->shm_dtime = (time_t) segment->detached_at;
		pgl_shm_unlock(registry);
		return 0;
	}
	if (cmd == IPC_SET && buf != NULL)
	{
		pgl_shm_unlock(registry);
		return 0;
	}

	pgl_shm_unlock(registry);
	errno = EINVAL;
	return -1;
}

#else

typedef struct ShmSegment
{
	int			shmid;
	key_t		key;
	size_t		size;
	void	   *addr;
	int			shmflg;
	struct ShmSegment *next;
}			ShmSegment;

static ShmSegment * shm_list = NULL;
static unsigned int next_shmid = 1;

/*  shmget replacement */
int			EMSCRIPTEN_KEEPALIVE
pgl_shmget(key_t key, size_t size, int shmflg)
{
	ShmSegment *seg = shm_list;

	/* Search for existing segment */
	while (seg)
	{
		if (seg->key == key)
			return seg->shmid;
		seg = seg->next;
	}

	/* If IPC_CREAT is set, create new segment */
	if (shmflg & IPC_CREAT)
	{
		int			pagesize = getpagesize();

		while (pagesize < size)
			pagesize += pagesize;
		void	   *mem = malloc(pagesize);

		if (!mem)
		{
			errno = ENOMEM;
			return -1;
		}

		ShmSegment *new_seg = malloc(sizeof(ShmSegment));

		if (!new_seg)
		{
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

/*  shmat replacement */
void		EMSCRIPTEN_KEEPALIVE
		   *
pgl_shmat(int shmid, const void *shmaddr, int shmflg)
{
	ShmSegment *seg = shm_list;

	while (seg)
	{
		if (seg->shmid == shmid)
		{
			return seg->addr;
		}
		seg = seg->next;
	}

	errno = EINVAL;
	return (void *) -1;
}

/*  shmdt replacement */
int			EMSCRIPTEN_KEEPALIVE
pgl_shmdt(const void *shmaddr)
{
	ShmSegment *seg = shm_list;

	while (seg)
	{
		if (seg->addr == shmaddr)
		{
			return 0;
		}
		seg = seg->next;
	}

	errno = EINVAL;
	return -1;
}

/*  shmctl replacement */
int			EMSCRIPTEN_KEEPALIVE
pgl_shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
	ShmSegment *seg = shm_list;
	ShmSegment *prev = NULL;

	while (seg)
	{
		if (seg->shmid == shmid)
		{
			if (cmd == IPC_RMID)
			{
				free(seg->addr);
				if (prev)
					prev->next = seg->next;
				else
					shm_list = seg->next;
				free(seg);
				return 0;
			}
			else if (cmd == IPC_STAT && buf != NULL)
			{
				buf->shm_segsz = seg->size;
				buf->shm_perm.__key = seg->key;
				buf->shm_nattch = 0;
				buf->shm_atime = buf->shm_dtime = buf->shm_ctime = time(NULL);
				return 0;
			}
			else if (cmd == IPC_SET && buf != NULL)
			{
				seg->size = buf->shm_segsz;
				return 0;
			}
			else
			{
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

void		EMSCRIPTEN_KEEPALIVE
pgl_set_shmem_host(int (*ensure_capacity) (uint32_t))
{
	(void) ensure_capacity;
}

#endif

/* ========== MMAP/MUNMAP ==========
 * Dummy munmap implementation for emscripten.
 * Emscripten's munmap can corrupt unrelated files in MEMFS,
 * so we just return success without doing anything.
 * Memory will be reclaimed when the WASM instance terminates.
 */
int			EMSCRIPTEN_KEEPALIVE
pgl_munmap(void *addr, size_t length)
{
	(void) addr;
	(void) length;
	/* dummy */
	return 0;
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
typedef ssize_t (*pgl_read_t) (void *buffer, size_t max_length);
pgl_read_t	pgl_read;

/* write TO JS
* Callback used for writing data to the frontend
*/
typedef ssize_t (*pgl_write_t) (void *buffer, size_t length);
pgl_write_t pgl_write;

/*
* Set the above callbacks
*/
void		EMSCRIPTEN_KEEPALIVE
pgl_set_rw_cbs(pgl_read_t read_cb, pgl_write_t write_cb)
{
	pgl_read = read_cb;
	pgl_write = write_cb;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_fcntl(int __fd, int __cmd,...)
{
	/* dummy  */
	return 0;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_setsockopt(int __fd, int __level, int __optname,
			   const void *__optval, socklen_t __optlen)
{
	/* dummy  */
	return 0;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_getsockopt(int __fd, int __level, int __optname,
			   void *__restrict __optval,
			   socklen_t *__restrict __optlen)
{
	/* dummy  */
	return 0;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_getsockname(int __fd, struct sockaddr *__addr,
				socklen_t *__restrict __len)
{
	/* dummy  */
	return 0;
}

/*
* Overrides the recv() libc function
*/

ssize_t		EMSCRIPTEN_KEEPALIVE
pgl_recv(int __fd, void *__buf, size_t __n, int __flags)
{
	if (pglite_recv != NULL)
		return pglite_recv(__fd, __buf, __n, __flags);
	if (pgl_read == NULL)
		return recv(__fd, __buf, __n, __flags);
	ssize_t		got = pgl_read(__buf, __n);

	return got;
}

/*
* Overrides the send() libc function
*/

ssize_t		EMSCRIPTEN_KEEPALIVE
pgl_send(int __fd, const void *__buf, size_t __n, int __flags)
{
	if (pglite_send != NULL)
		return pglite_send(__fd, __buf, __n, __flags);
	if (pgl_write == NULL)
		return send(__fd, __buf, __n, __flags);
	ssize_t		wrote = pgl_write(__buf, __n);

	return wrote;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_connect(int socket, const struct sockaddr *address, socklen_t address_len)
{
#ifdef __PGLITE_POSTMASTER__
	return connect(socket, address, address_len);
#else
	/* dummy */
	return 0;
#endif
}

int			EMSCRIPTEN_KEEPALIVE
pgl_socket(int domain, int type, int protocol)
{
	if (pglite_socket != NULL)
		return pglite_socket(domain, type, protocol);
	return socket(domain, type, protocol);
}

int			EMSCRIPTEN_KEEPALIVE
pgl_bind(int socket_fd, const struct sockaddr *address,
		 socklen_t address_len)
{
	if (pglite_bind != NULL)
		return pglite_bind(socket_fd, address, address_len);
	return bind(socket_fd, address, address_len);
}

int			EMSCRIPTEN_KEEPALIVE
pgl_listen(int socket_fd, int backlog)
{
	if (pglite_listen != NULL)
		return pglite_listen(socket_fd, backlog);
	return listen(socket_fd, backlog);
}

int			EMSCRIPTEN_KEEPALIVE
pgl_accept(int socket_fd, struct sockaddr *address,
		   socklen_t *address_len)
{
	if (pglite_accept != NULL)
		return pglite_accept(socket_fd, address, address_len);
	return accept(socket_fd, address, address_len);
}

int			EMSCRIPTEN_KEEPALIVE
pgl_close(int fd)
{
	int			result;

	if (pglite_close == NULL)
		return close(fd);
	result = pglite_close(fd);
	if (result == PGL_SOCKET_NOT_HANDLED)
		return close(fd);
	return result;
}

int			EMSCRIPTEN_KEEPALIVE
pgl_poll(struct pollfd fds[], nfds_t nfds, int timeout)
{
#ifdef __PGLITE_POSTMASTER__
	int			result;

	if (pglite_poll != NULL)
		result = pglite_poll(fds, nfds, timeout);
	else
		result = poll(fds, nfds, timeout);

	/*
	 * Native signal handlers run asynchronously while poll(2) is blocked.
	 * The Worker host instead wakes the poll callback with EINTR and queues
	 * the signal in the process-control SAB.  Dispatch at that same boundary
	 * before PostgreSQL retries its wait, so handlers can set their latches
	 * and flags.
	 */
	pgl_dispatch_pending_signals();
	return result;
#else
	/* The single-user input pump reports its one emulated socket as ready. */
	return nfds;
#endif
}
