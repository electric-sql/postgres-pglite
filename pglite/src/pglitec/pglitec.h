#ifndef PGLITEC_H
#define PGLITEC_H

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
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

pid_t pgl_spawn_backend(const char *child_kind, const char *parameter_file,
						int client_socket);
pid_t pgl_getpid(void);
int pgl_kill(pid_t pid, int signal_number);
pid_t pgl_waitpid(pid_t pid, int *status, int options);
int pgl_setitimer(int which, const struct itimerval *value,
				  struct itimerval *old_value);
int pgl_sigaction(int signal_number, const struct sigaction *action,
				  struct sigaction *old_action);
int pgl_sigprocmask(int how, const sigset_t *set, sigset_t *old_set);
void pgl_dispatch_pending_signals(void);
int pgl_futex_wait(void *address, uint32_t expected, double timeout_ms);
int pgl_futex_wake(void *address, int count);
int pgl_socket(int domain, int type, int protocol);
int pgl_bind(int socket_fd, const struct sockaddr *address,
			 socklen_t address_len);
int pgl_listen(int socket_fd, int backlog);
int pgl_accept(int socket_fd, struct sockaddr *address,
			   socklen_t *address_len);
int pgl_close(int fd);
int pgl_poll(struct pollfd *fds, nfds_t nfds, int timeout);
void pgl_set_shmem_host(int (*ensure_capacity)(uint32_t));
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
