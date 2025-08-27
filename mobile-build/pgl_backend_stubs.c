#include <stdbool.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "postgres.h"
#include "miscadmin.h"          // BackendType, sig_atomic_t globals
#include "storage/latch.h"      // Latch, MyLatch
#include "postmaster/bgworker.h"// BackgroundWorker
#include "libpq/libpq-be.h"   // Port definition

#include "utils/timeout.h"      // TimeoutId, TimestampTz
#include "storage/shmem.h"      // Size, ShmemInitStruct

// Provide weak default definitions so real backend objects can override if linked
volatile sig_atomic_t InterruptPending __attribute__((weak)) = 0;
volatile sig_atomic_t QueryCancelPending __attribute__((weak)) = 0;
volatile sig_atomic_t ProcDiePending __attribute__((weak)) = 0;
volatile bool ClientAuthInProgress __attribute__((weak)) = false;
volatile bool notifyInterruptPending __attribute__((weak)) = false;
volatile bool catchupInterruptPending __attribute__((weak)) = 0;
volatile sig_atomic_t CheckClientConnectionPending __attribute__((weak)) = 0;
volatile sig_atomic_t ClientConnectionLost __attribute__((weak)) = 0;

// Holdoff counters used by ProcessInterrupts (match miscadmin.h types)
volatile uint32 InterruptHoldoffCount __attribute__((weak)) = 0;
volatile uint32 CritSectionCount __attribute__((weak)) = 0;
volatile uint32 QueryCancelHoldoffCount __attribute__((weak)) = 0;

// Timeout indicators used by ProcessInterrupts (match miscadmin.h types)
volatile sig_atomic_t IdleInTransactionSessionTimeoutPending __attribute__((weak)) = 0;
volatile sig_atomic_t TransactionTimeoutPending __attribute__((weak)) = 0;
volatile sig_atomic_t IdleSessionTimeoutPending __attribute__((weak)) = 0;
volatile sig_atomic_t IdleStatsUpdateTimeoutPending __attribute__((weak)) = 0;

BackendType MyBackendType __attribute__((weak)) = B_BACKEND;
BackgroundWorker *MyBgworkerEntry __attribute__((weak)) = NULL;

// Provide a basic latch instance so pointer is valid
static Latch s_MyLatch = {0};
Latch *MyLatch __attribute__((weak)) = &s_MyLatch;

// Parallel / barrier / logging related flags
volatile sig_atomic_t ProcSignalBarrierPending __attribute__((weak)) = 0;
bool ParallelMessagePending __attribute__((weak)) = false;
bool ParallelApplyMessagePending __attribute__((weak)) = false;
volatile sig_atomic_t LogMemoryContextPending __attribute__((weak)) = 0;

// Protocol state variables used by interactive_one.c
volatile bool ignore_till_sync __attribute__((weak)) = false;
volatile bool send_ready_for_query __attribute__((weak)) = true;

// Additional missing variables for mobile build
#ifndef EOF
#define EOF (-1)
#endif

// Parser stats GUC
bool log_parser_stats __attribute__((weak)) = false;

// Max backends (used by shared memory sizing helpers)
int MaxBackends __attribute__((weak)) = 64;

// Minimal implementations
void __attribute__((weak)) ProcessNotifyInterrupt(bool flush) { (void)flush; }
void __attribute__((weak)) ProcessCatchupInterrupt(void) {}
void __attribute__((weak)) ProcessLogMemoryContextInterrupt(void) {}
void __attribute__((weak)) HandleParallelMessages(void) {}
void __attribute__((weak)) HandleParallelApplyMessages(void) {}

bool __attribute__((weak)) pq_check_connection(void) { return true; }
bool __attribute__((weak)) IsTransactionOrTransactionBlock(void) { return false; }

void __attribute__((weak)) enable_timeout_after(TimeoutId id, int delay) { (void)id; (void)delay; }
bool __attribute__((weak)) get_timeout_indicator(TimeoutId id, bool reset) { (void)id; (void)reset; return false; }
TimestampTz __attribute__((weak)) get_timeout_finish_time(TimeoutId id) { (void)id; return 0; }

// Shared memory helpers stubs (satisfy link-time deps from sinval/procsignal)

// Timeouts: provide safe no-op implementations used during startup/single-user
void disable_timeout(TimeoutId id, bool keep_indicator) { (void)id; (void)keep_indicator; }
void disable_all_timeouts(bool keep_indicator) { (void)keep_indicator; }
void reschedule_timeouts(void) {}

void * __attribute__((weak)) ShmemInitStruct(const char *name, Size size, bool *found)
{
  (void)name; (void)size; if (found) *found = true; static int dummy; return &dummy;
}

// Safe size helpers (normally in libpgcommon)
size_t __attribute__((weak)) add_size(size_t a, size_t b) { return a + b; }
size_t __attribute__((weak)) mul_size(size_t a, size_t b) { return a * b; }

// Stats reporting stub
void __attribute__((weak)) pgstat_report_stat(bool force) { (void)force; }

// Graceful exit hook used by pg_shutdown and bootstrap
// Intercept proc_exit during bootstrap by longjmping to pgl_boot_jmp if set
#include <setjmp.h>
extern volatile sigjmp_buf* pgl_boot_jmp; // defined in pg_main.c
void proc_exit(int code) {
  (void)code;
  if (pgl_boot_jmp) {
    // Jump back to caller to avoid PANIC/exit in bootstrap context
    siglongjmp(*(sigjmp_buf*)pgl_boot_jmp, 1);
  }
  // Outside bootstrap, do nothing — avoid exiting the host process.
}

// Minimal pq_init to avoid socket syscalls in in-process mobile mode
// PG16+ uses void pq_init(void); older versions return Port*
#if defined(PG_VERSION_NUM) && (PG_VERSION_NUM >= 160000)
void pq_init(void) {
  if (!MyProcPort) {
    MyProcPort = (Port *) calloc(1, sizeof(Port));
    if (MyProcPort) {
      MyProcPort->sock = -1; // not a real socket
    }
  }
  // Leave whereToSendOutput to be set by caller (interactive_one)
}
#else
Port* pq_init(ClientSocket *server_fd) {
  (void)server_fd;
  if (!MyProcPort) {
    MyProcPort = (Port *) calloc(1, sizeof(Port));
    if (MyProcPort) {
      MyProcPort->sock = -1;
    }
  }
  return MyProcPort;
}
#endif

// Ensure MyProcPort exists early so interactive_one() skips io_init()/pq_init
__attribute__((constructor)) static void pgl_mobile_preinit(void) {
  if (!MyProcPort) {
    MyProcPort = (Port *) calloc(1, sizeof(Port));
    if (MyProcPort) {
      MyProcPort->sock = -1; // mark as non-socket
    }
  }
}


void pg_proc_exit(int code) { proc_exit(code); }

// WASM pipe emulation symbols expected by interactive_one.c
FILE * __attribute__((weak)) SOCKET_FILE = NULL;
int __attribute__((weak)) SOCKET_DATA = 0;
void __attribute__((weak)) pq_recvbuf_fill(FILE* fp, int packetlen) { (void)fp; (void)packetlen; }

// Removed pq_getbyte and pq_getbytes overrides - these should NOT be overridden!
// PostgreSQL's pq_getbyte/pq_getbytes work through PQcommMethods which we've
// already set up in pgl_mobile_comm.c. Overriding them directly breaks bootstrap
// mode where regular file I/O is needed.

// Logical replication worker queries
bool __attribute__((weak)) IsLogicalWorker(void) { return false; }
bool __attribute__((weak)) IsLogicalLauncher(void) { return false; }

