#ifndef PGLITE_BACKEND_LAUNCH_H
#define PGLITE_BACKEND_LAUNCH_H

#include "postmaster/bgworker.h"
#include "pglitec.h"

static inline pid_t
pgl_launch_backend(const char *child_kind, const char *parameter_file,
				   int client_socket, const void *startup_data,
				   size_t startup_data_len)
{
	pid_t leader_pid = 0;

	/* Dynamic workers share their registering backend's memory 2. */
	if (strcmp(child_kind, "bgworker") == 0 &&
		startup_data_len == sizeof(BackgroundWorker))
	{
		const BackgroundWorker *worker = startup_data;

		if (worker->bgw_notify_pid > 0)
			leader_pid = worker->bgw_notify_pid;
	}
	return pgl_spawn_backend(child_kind, parameter_file, client_socket,
						 leader_pid);
}

#endif
