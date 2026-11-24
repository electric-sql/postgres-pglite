/*-------------------------------------------------------------------------
 *
 * pglitec_launch.c
 *	  Capture the parameters of a postmaster child launch
 *
 * NOTES
 *    Linked with -Wl,--wrap=postmaster_child_launch: every call to
 *    postmaster_child_launch() made by Postgres lands here first.
 *
 *    With EXEC_BACKEND, postmaster_child_launch() writes the child's
 *    parameters to a temp file and then calls fork() + execv(). Our fork()
 *    (pgl_fork in pglitec.c) takes no arguments, so we record here what
 *    execv() would have received: the child kind and the parameter file.
 *
 *    This lives in its own object file so that it is only pulled into the
 *    backend link; the other programs linking libpglitec.a do not have
 *    postmaster_child_launch().
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>

/* launch parameters consumed by pgl_fork() */
extern int pglite_child_type;
extern const char *pglite_child_kind;
extern char pglite_child_param_file[1024];
extern size_t pglite_child_startup_data_len;

extern pid_t pgl_getpid(void);

/* from Postgres */
extern const char *PostmasterChildName(int child_type);
extern pid_t __real_postmaster_child_launch(int child_type,
											char *startup_data,
											size_t startup_data_len,
											void *client_sock);

/*
 * Mirrors the counter in internal_forkexec() (launch_backend.c), which
 * names the parameter file
 *   PG_TEMP_FILES_DIR/PG_TEMP_FILE_PREFIX.backend_var.<pid>.<counter>
 * and increments the counter once per launch.
 */
static unsigned long backend_file_num = 0;

pid_t
__wrap_postmaster_child_launch(int child_type,
							   char *startup_data,
							   size_t startup_data_len,
							   void *client_sock)
{
	pid_t		pid;

	pglite_child_type = child_type;
	pglite_child_kind = PostmasterChildName(child_type);
	pglite_child_startup_data_len = startup_data_len;
	snprintf(pglite_child_param_file, sizeof(pglite_child_param_file),
			 "pgsql_tmp/pgsql_tmp.backend_var.%d.%lu",
			 (int) pgl_getpid(), ++backend_file_num);

	pid = __real_postmaster_child_launch(child_type, startup_data,
										 startup_data_len, client_sock);

	pglite_child_kind = NULL;
	return pid;
}

/*
 * Linked with -Wl,--wrap=find_other_exec.
 *
 * With EXEC_BACKEND the postmaster looks for the postgres executable it will
 * start its children from, and checks its version by running "postgres -V".
 * Children are new instances of the very module that is running, so there is
 * nothing to look for: answer with our own path.
 */
extern char my_exec_path[];

int
__wrap_find_other_exec(const char *argv0, const char *target,
					   const char *versionstr, char *retpath)
{
	(void) argv0;
	(void) target;
	(void) versionstr;
	snprintf(retpath, 1024 /* MAXPGPATH */ , "%s", my_exec_path);
	return 0;
}
