#ifndef PGLITEC_H
#define PGLITEC_H

#include <poll.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PGL_SOCKET_NOT_HANDLED (-2)

int pgl_find_other_exec(const char *argv0, const char *target,
						const char *version_string, char *result,
						size_t result_size);
int pgl_readdir_includes_dot_entries(void);
int pgl_getaddrinfo(const char *host, const char *service,
					const struct addrinfo *hints, struct addrinfo **result);
struct passwd *pgl_getpwuid(uid_t uid);
int pgl_getpwuid_r(uid_t uid, struct passwd *pwd, char *buffer,
				   size_t buffer_size, struct passwd **result);
#ifdef __PGLITE_POSTMASTER__
uintptr_t pgl_stack_get_current(void);
uintptr_t pgl_heap_break(void);
static inline int
pgl_uses_unrolled_main_loop(void)
{
	return 0;
}
#else
int pgl_uses_unrolled_main_loop(void);
#endif

pid_t pgl_spawn_backend(const char *child_kind, const char *parameter_file,
						int client_socket, pid_t scope_leader_pid);
pid_t pgl_getpid(void);
int pgl_kill(pid_t pid, int signal_number);
pid_t pgl_waitpid(pid_t pid, int *status, int options);
int pgl_setitimer(int which, const struct itimerval *value,
				  struct itimerval *old_value);
int pgl_sigaction(int signal_number, const struct sigaction *action,
				  struct sigaction *old_action);
int pgl_sigprocmask(int how, const sigset_t *set, sigset_t *old_set);
void pgl_dispatch_pending_signals(void);
void pgl_notify_latch_owner(pid_t owner_pid);
int pgl_gettimeofday(struct timeval *time_value, void *timezone_value);
void pgl_set_clock_host(int64_t (*realtime_microseconds)(void));
#ifdef __PGLITE_POSTMASTER__
void *pgl_dlopen(const char *filename, int flags);
void *pgl_dlsym(void *handle, const char *name);
int pgl_dlclose(void *handle);
int pgl_dlopen_stat(const char *filename, struct stat *stat_buf);
#ifndef PGLITEC_IMPLEMENTATION
#define dlopen pgl_dlopen
#define dlsym pgl_dlsym
#define dlclose pgl_dlclose
#endif
#endif
int pgl_futex_wait(void *address, uint32_t expected, double timeout_ms);
int pgl_futex_wake(void *address, int count);
int pgl_socket(int domain, int type, int protocol);
int pgl_bind(int socket_fd, const struct sockaddr *address,
			 socklen_t address_len);
int pgl_configure_unix_socket(int socket_fd, const char *path,
							  const char *group, unsigned int mode);
int pgl_listen(int socket_fd, int backlog);
int pgl_accept(int socket_fd, struct sockaddr *address,
			   socklen_t *address_len);
int pgl_close(int fd);
int pgl_poll(struct pollfd *fds, nfds_t nfds, int timeout);
void pgl_set_shmem_host(int (*ensure_capacity)(uint32_t));
void pgl_set_scoped_shmem_host(int (*ensure_capacity)(uint32_t));

typedef enum PglScopedShmemMode
{
	PGL_SCOPED_SHMEM_DISABLED = 0,
	PGL_SCOPED_SHMEM_DEDICATED,
	PGL_SCOPED_SHMEM_COMPACT
} PglScopedShmemMode;

void pgl_set_scoped_shmem_mode(int mode);
uint32_t pgl_shm_registry_offset(void);
uint32_t pgl_shm_compact_frontier(void);

typedef enum PglSharedScopeKind
{
	PGL_SHARED_SCOPE_GLOBAL = 0,
	PGL_SHARED_SCOPE_ROOT,
	PGL_SHARED_SCOPE_SESSION,
	PGL_SHARED_SCOPE_TRANSACTION,
	PGL_SHARED_SCOPE_SUBTRANSACTION,
	PGL_SHARED_SCOPE_PORTAL,
	PGL_SHARED_SCOPE_QUERY,
	PGL_SHARED_SCOPE_PARALLEL_CONTEXT
} PglSharedScopeKind;

typedef uint64_t PglSharedScopeHandle;

#define PGL_SHARED_SCOPE_INVALID UINT64_C(0)

typedef enum PglSharedScopeState
{
	PGL_SHARED_SCOPE_UNUSED = 0,
	PGL_SHARED_SCOPE_ACTIVE,
	PGL_SHARED_SCOPE_CLOSING,
	PGL_SHARED_SCOPE_DEAD
} PglSharedScopeState;

PglSharedScopeHandle pgl_shm_scope_root(void);
PglSharedScopeHandle pgl_shm_scope_create(PglSharedScopeKind kind,
										  PglSharedScopeHandle parent);
PglSharedScopeHandle pgl_shm_scope_enter(PglSharedScopeHandle scope);
void pgl_shm_scope_leave(PglSharedScopeHandle previous_scope);
int pgl_shm_scope_close(PglSharedScopeHandle scope);
int pgl_shm_scope_promote(PglSharedScopeHandle scope);
int pgl_shm_scope_worker_attach(PglSharedScopeHandle scope);
int pgl_shm_scope_worker_detach(PglSharedScopeHandle scope);
PglSharedScopeHandle pgl_shm_scope_current(void);
PglSharedScopeHandle pgl_shm_scope_handle_for_pointer(const void *address);
uint32_t pgl_shm_scope_count(PglSharedScopeKind kind,
							 PglSharedScopeState state);
uint64_t pgl_shm_scope_bytes(PglSharedScopeKind kind);
PglSharedScopeKind pgl_shm_scope_push(PglSharedScopeKind scope);
void pgl_shm_scope_pop(PglSharedScopeKind previous_scope);
PglSharedScopeKind pgl_shm_scope_for_pointer(const void *address);
ssize_t pgl_fd_read(int fd, void *buffer, size_t length);
ssize_t pgl_fd_write(int fd, const void *buffer, size_t length);
ssize_t pgl_fd_pread(int fd, void *buffer, size_t length, off_t offset);
ssize_t pgl_fd_pwrite(int fd, const void *buffer, size_t length, off_t offset);
ssize_t pgl_fd_readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t pgl_fd_writev(int fd, const struct iovec *iov, int iovcnt);
ssize_t pgl_fd_preadv(int fd, const struct iovec *iov, int iovcnt,
					  off_t offset);
ssize_t pgl_fd_pwritev(int fd, const struct iovec *iov, int iovcnt,
					   off_t offset);

#ifdef __cplusplus
}
#endif

#endif
