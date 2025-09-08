#include "pgl_mobile_compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "../pglite-wasm/pgl_os.h"

// Provide a stable mobile symbol name expected by the RN bridge.
// Map pgl_shutdown() to the implementation in pgl_mains.c (pg_shutdown).
extern void pg_shutdown(void);
void pgl_shutdown(void) { pg_shutdown(); }

static int file_exists(const char* p) {
  struct stat st; return (p && stat(p, &st) == 0 && S_ISREG(st.st_mode));
}
static int dir_exists(const char* p) {
  struct stat st; return (p && stat(p, &st) == 0 && S_ISDIR(st.st_mode));
}

// Stub find_other_exec to avoid probing the filesystem for real binaries.
// We always "find" postgres next to PREFIX/bin/postgres.
int find_other_exec(const char *argv0, const char *target, const char *versionstr, char *retpath) {
  (void)argv0; (void)versionstr;
  const char* prefix = getenv("PREFIX");
  if (!prefix || !*prefix) prefix = getenv("ANDROID_DATA_DIR");
  if (!prefix || !*prefix) prefix = "/data/local/tmp/pglite";
  // Compose PREFIX/bin/<target>
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s/bin/%s", prefix, target ? target : "postgres");
  strcpy(retpath, buf);
  PGL_LOG_INFO("find_other_exec: argv0=%s target=%s ret=%s prefix=%s", argv0?argv0:"", target?target:"", retpath, prefix);
  return 0; // success
}


// Pretend we can always locate our own executable; set to PREFIX/bin/postgres
int find_my_exec(const char *argv0, char *retpath) {
  (void)argv0;
  const char* prefix = getenv("PREFIX");
  if (!prefix || !*prefix) prefix = getenv("ANDROID_DATA_DIR");
  if (!prefix || !*prefix) prefix = "/data/local/tmp/pglite";
  snprintf(retpath, 1024, "%s/bin/postgres", prefix);
  PGL_LOG_INFO("find_my_exec: argv0=%s ret=%s prefix=%s", argv0?argv0:"", retpath, prefix);
  return 0; // success
}

// Make initdb use our packaged catalogs instead of deriving from executable path.
// Prefer PGSYSCONFDIR if set; otherwise derive from platform runtime directory.
void get_share_path(const char *my_exec_path, char *ret_path) {
  (void)my_exec_path;
  const char* conf = getenv("PGSYSCONFDIR");
#ifdef __APPLE__
  const char* runtime = getenv("IOS_RUNTIME_DIR");
#else
  const char* runtime = getenv("ANDROID_RUNTIME_DIR");
#endif

  // Candidates in order: PGSYSCONFDIR, runtime/share/postgresql, runtime/postgresql
  char cand1[1024] = {0}, cand2[1024] = {0}, cand3[1024] = {0};
  if (conf && *conf) snprintf(cand1, sizeof(cand1), "%s", conf);
  if (runtime && *runtime) {
    snprintf(cand2, sizeof(cand2), "%s/share/postgresql", runtime);
    snprintf(cand3, sizeof(cand3), "%s/postgresql", runtime);
  }
  // We need the directory that directly contains postgres.bki
  char bki1[1200] = {0}, bki2[1200] = {0}, bki3[1200] = {0};
  if (*cand1) { snprintf(bki1, sizeof(bki1), "%s/postgres.bki", cand1); }
  if (*cand2) { snprintf(bki2, sizeof(bki2), "%s/postgres.bki", cand2); }
  if (*cand3) { snprintf(bki3, sizeof(bki3), "%s/postgres.bki", cand3); }

  PGL_LOG_INFO("get_share_path: conf=%s runtime=%s cand1=%s cand2=%s cand3=%s exists(bki1)=%d exists(bki2)=%d exists(bki3)=%d",
    conf?conf:"", runtime?runtime:"", cand1, cand2, cand3, file_exists(bki1), file_exists(bki2), file_exists(bki3));

  if (*cand1 && file_exists(bki1)) { strcpy(ret_path, cand1); goto done; }
  if (*cand2 && file_exists(bki2)) { strcpy(ret_path, cand2); goto done; }
  if (*cand3 && file_exists(bki3)) { strcpy(ret_path, cand3); goto done; }
  // Fallback: use conf if set, else runtime/share/postgresql even if empty
  if (*cand1) { strcpy(ret_path, cand1); goto done; }
  if (*cand2) { strcpy(ret_path, cand2); goto done; }
  if (*cand3) { strcpy(ret_path, cand3); goto done; }
  // Last resort
  strcpy(ret_path, "/data/local/tmp/pglite/share/postgresql");

done:
  PGL_LOG_INFO("get_share_path: chosen=%s", ret_path);
}

