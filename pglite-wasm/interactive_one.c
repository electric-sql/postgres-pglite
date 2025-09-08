#include <unistd.h>  // access, unlink
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>

#include <sys/stat.h> // fstat
#include <errno.h>
#define PGL_LOOP
// Unified logging - PGL_LOG_INFO replaced with PGL_LOG_INFO
#include "pgl_os.h"  // Provides unified logging macros (PGL_LOG_INFO, etc.)

#if defined(__wasi__)
// volatile sigjmp_buf void*;
#else
volatile sigjmp_buf local_sigjmp_buf;
#endif

// track back how many ex raised in steps of the loop until sucessfull clear_error
volatile int canary_ex = 0;

// track back mode used for last reply   <0 socketfiles , 0== repl , > 0 cma addr
volatile int channel = 0;

/* TODO : prevent multiple write and write while reading ? */
#ifdef PGL_MOBILE
#include "sdk_port-mobile.h"
#include "pgl_os.h"
static bool mobile_auth_started = false;

#define MOBILE_LOG_CALL(func_name) PGL_LOG_INFO("CALL: %s() at line %d", func_name, __LINE__)
#endif

#ifndef PGL_MOBILE
volatile int cma_wsize = 0;
volatile int cma_rsize = 0;  // also defined in postgres.c for pqcomm
#else
/* On mobile, these are external variables defined in pqcomm.c */
extern volatile int pgl_mobile_cma_wsize;
extern volatile int cma_rsize;
#define cma_wsize pgl_mobile_cma_wsize
#endif
volatile bool sockfiles = false; // also defined in postgres.c for pqcomm

#if !defined(PGL_MOBILE)
__attribute__((export_name("get_buffer_size")))
int
get_buffer_size(int fd) {
    return (CMA_MB * 1024 * 1024) / CMA_FD;
}

// TODO add query size
__attribute__((export_name("get_buffer_addr")))
int
get_buffer_addr(int fd) {
    return 1 + ( get_buffer_size(fd) *fd);
}
#endif

__attribute__((export_name("get_channel")))
int
get_channel() {
    return channel;
}


__attribute__((export_name("interactive_read")))
int
interactive_read() {
    /* should cma_rsize should be reset here ? */
    return cma_wsize;
}


static void pg_prompt() {
    fprintf(stdout,"pg> %c\n", 4);
}

extern void AbortTransaction(void);
extern void CleanupTransaction(void);
extern void ClientAuthentication(Port *port);
extern FILE* SOCKET_FILE;
extern int SOCKET_DATA;



/*
init sequence
___________________________________
SubPostmasterMain / (forkexec)
    InitPostmasterChild
    shm attach
    preload

    BackendInitialize(Port *port) -> collect initial packet

	    pq_init();
	    whereToSendOutput = DestRemote;
	    status = ProcessStartupPacket(port, false, false);
            pq_startmsgread
            pq_getbytes from pq_recvbuf
            TODO: place PqRecvBuffer (8K) in lower mem for zero copy

        PerformAuthentication
        ClientAuthentication(port)
        CheckPasswordAuth SYNC!!!!  ( sendAuthRequest flush -> recv_password_packet )
    InitShmemAccess/InitProcess/CreateSharedMemoryAndSemaphores

    BackendRun(port)
        PostgresMain


-> pq_flush() is synchronous


buffer sizes:

    https://github.com/postgres/postgres/blob/master/src/backend/libpq/pqcomm.c#L118

    https://github.com/postgres/postgres/blob/master/src/common/stringinfo.c#L28


*/

extern int	ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done);
extern void pq_recvbuf_fill(FILE* fp, int packetlen);

#define PG_MAX_AUTH_TOKEN_LENGTH	65535
static char *
recv_password_packet(Port *port) {
	StringInfoData buf;
	int			mtype;

	pq_startmsgread();

	/* Expect 'p' message type */
	mtype = pq_getbyte();
	if (mtype != 'p')
	{
		/*
		 * If the client just disconnects without offering a password, don't
		 * make a log entry.  This is legal per protocol spec and in fact
		 * commonly done by psql, so complaining just clutters the log.
		 */
		if (mtype != EOF)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("expected password response, got message type %d",
							mtype)));
		return NULL;			/* EOF or bad message type */
	}

	initStringInfo(&buf);
	if (pq_getmessage(&buf, PG_MAX_AUTH_TOKEN_LENGTH))	/* receive password */
	{
		/* EOF - pq_getmessage already logged a suitable message */
		pfree(buf.data);
		return NULL;
	}

	/*
	 * Apply sanity check: password packet length should agree with length of
	 * contained string.  Note it is safe to use strlen here because
	 * StringInfo is guaranteed to have an appended '\0'.
	 */
	if (strlen(buf.data) + 1 != buf.len)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid password packet size")));

	/*
	 * Don't allow an empty password. Libpq treats an empty password the same
	 * as no password at all, and won't even try to authenticate. But other
	 * clients might, so allowing it would be confusing.
	 *
	 * Note that this only catches an empty password sent by the client in
	 * plaintext. There's also a check in CREATE/ALTER USER that prevents an
	 * empty string from being stored as a user's password in the first place.
	 * We rely on that for MD5 and SCRAM authentication, but we still need
	 * this check here, to prevent an empty password from being used with
	 * authentication methods that check the password against an external
	 * system, like PAM, LDAP and RADIUS.
	 */
	if (buf.len == 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PASSWORD),
				 errmsg("empty password returned by client")));

	/* Do not echo password to logs, for security. */
	elog(DEBUG5, "received password packet");
	return buf.data;
}


int md5Salt_len  = 4;
char md5Salt[4];
ClientSocket dummy_sock;

static void io_init(bool in_auth, bool out_auth) {
    ClientAuthInProgress = in_auth;
#ifdef PG16

	pq_init();					/* initialize libpq to talk to client */
    MyProcPort = (Port *) calloc(1, sizeof(Port));
#else
#ifdef PGL_MOBILE
    if (is_wire && MyProcPort) {
        PGL_LOG_INFO("PG17: CMA mode, no socket needed");
        MyProcPort->sock = -1; // No real socket in CMA mode
    } else {
        PGL_LOG_INFO("PG17: skipped assign sock, MyProcPort=%p CMA mode is_wire=%d",
                   (void*)MyProcPort, (int)is_wire);
    }
#endif

#ifdef PGL_MOBILE
    /* Socketpair will be created in per-request setup */
    dummy_sock.sock = -1;
#endif
    MyProcPort = pq_init(&dummy_sock);
    /* On mobile, pq_init may be a no-op; allocate Port if still NULL */
    if (!MyProcPort) {
        MyProcPort = (Port *) calloc(1, sizeof(Port));
        /* If we created a socketpair, attach it */
#ifdef PGL_MOBILE
        if (is_wire) {
            MyProcPort->sock = -1; // CMA mode, no real socket
        }
#endif
    }
#endif
	whereToSendOutput = DestRemote; /* now safe to ereport to client */
#ifdef PGL_MOBILE
    PGL_LOG_INFO("io_init: MyProcPort=%p sock(pre)=%d is_wire=%d", (void*)MyProcPort, MyProcPort?MyProcPort->sock:-1, (int)is_wire);
#endif
    PGL_LOG_INFO("io_init(univ): port=%p sock=%d", (void*)MyProcPort, MyProcPort?MyProcPort->sock:-1);

    if (!MyProcPort) {
        PDEBUG("# 155: io_init   --------- NO CLIENT (oom) ---------");
        abort();
    }
#ifdef PG16
    MyProcPort->canAcceptConnections = CAC_OK;
#endif
    ClientAuthInProgress = out_auth;
    SOCKET_FILE = NULL;
    SOCKET_DATA = 0;
#ifdef PGL_MOBILE
    if (is_wire) {
        /* CMA mode: data is already in buffer, no socket writing needed */
        if (cma_rsize > 0) {
            PGL_LOG_INFO("io_init: CMA buffer has %d bytes ready", cma_rsize);
        }
    }
#endif
    PDEBUG("\n\n\n# 165: io_init  --------- Ready for CLIENT ---------");
}


#ifdef PGL_MOBILE
/* Mobile: these are defined in sdk_port-mobile.c */
extern volatile bool is_wire;
extern volatile bool is_repl;
#else
/* WASM: define here */
volatile bool is_wire = true;
extern volatile bool is_repl;  /* Defined in pg_main.c for WASM */
#endif
extern char * cma_port;
extern void pq_startmsgread(void);

__attribute__((export_name("interactive_write")))
void
interactive_write(int size) {
    cma_rsize = size;
#ifndef PGL_MOBILE
    cma_wsize = 0;
#else
    pgl_mobile_cma_wsize = 0;
    extern int original_request_size;
    original_request_size = size;  /* Save for mobile_flush offset calculation */
    PGL_LOG_INFO("interactive_write: saved original_request_size=%d", original_request_size);
#endif
}

__attribute__((export_name("use_wire")))
void
use_wire(int state) {
#if PGDEBUG
    force_echo=true;
#endif
    if (state>0) {
#if PGDEBUG
        printf("\n\n# 194: PACKET START: wire mode, repl off, echo %d\n", force_echo);
#endif
        is_wire = true;
        is_repl = false;
    } else {
#if PGDEBUG
        printf("\n\n# 200: PACKET START: repl mode, no wire, echo %d\n", force_echo);
#endif
        is_wire = false;
        is_repl = true;
    }
}

__attribute__((export_name("clear_error")))
void
clear_error() {
    error_context_stack = NULL;
    HOLD_INTERRUPTS();

    disable_all_timeouts(false);	/* do first to avoid race condition */
    QueryCancelPending = false;
    idle_in_transaction_timeout_enabled = false;
    idle_session_timeout_enabled = false;
    DoingCommandRead = false;
puts("# 239:" __FILE__ );
    pq_comm_reset();
    EmitErrorReport();
    debug_query_string = NULL;
puts("# 243:" __FILE__ );
    AbortCurrentTransaction();

    if (am_walsender)
        WalSndErrorCleanup();

    PortalErrorCleanup();
    if (MyReplicationSlot != NULL)
        ReplicationSlotRelease();
#ifdef PG16
    ReplicationSlotCleanup();
#else
    ReplicationSlotCleanup(false);
#endif

    MemoryContextSwitchTo(TopMemoryContext);
    FlushErrorState();

    if (doing_extended_query_message)
        ignore_till_sync = true;

    xact_started = false;

    if (pq_is_reading_msg())
        ereport(FATAL,
                (errcode(ERRCODE_PROTOCOL_VIOLATION),
                 errmsg("terminating connection because protocol synchronization was lost")));

    RESUME_INTERRUPTS();

    /*
     * If we were handling an extended-query-protocol message, skip till next Sync.
     * This also causes us not to issue ReadyForQuery (until we get Sync).
     */

    if (!ignore_till_sync)
        send_ready_for_query = true;
}

void discard_input(){
    if (!cma_rsize)
        return;
    pq_startmsgread();
    for (int i = 0; i < cma_rsize; i++) {
        pq_getbyte();
    }
    pq_endmsgread();
}

void
startup_auth() {
    PGL_LOG_INFO("startup_auth: enter, port=%p sock=%d", (void*)MyProcPort, MyProcPort?MyProcPort->sock:-1);
    /* code is in handshake/auth domain so read whole msg now */
    send_ready_for_query = false;
#ifdef PGL_MOBILE
    /* CMA mode: data is already in buffer, no socket writing needed */
    if (cma_rsize > 0) {
        PGL_LOG_INFO("startup_auth: CMA buffer has %d bytes ready", cma_rsize);
    }
#endif

    if (ProcessStartupPacket(MyProcPort, true, true) != STATUS_OK) {
        PDEBUG("# 271: ProcessStartupPacket !OK");
    } else {

        sf_connected++;
        PDEBUG("# 273: sending auth request");
        //ClientAuthentication(MyProcPort);
        discard_input();

ClientAuthInProgress = true;
        md5Salt[0]=0x01;
        md5Salt[1]=0x23;
        md5Salt[2]=0x45;
        md5Salt[3]=0x56;
        {
            StringInfoData buf;
            pq_beginmessage(&buf, 'R');
            pq_sendint32(&buf, (int32) AUTH_REQ_MD5);
            if (md5Salt_len > 0)
                pq_sendbytes(&buf, md5Salt, md5Salt_len);
            pq_endmessage(&buf);
            pq_flush();
        }
    }
}


void
startup_pass(bool check) {
    // auth 'p'
    if (check) {
        char *passwd = recv_password_packet(MyProcPort);
        PDEBUG("# 223: auth recv password: md5***");
        /*
        // TODO: CheckMD5Auth
            if (passwd == NULL)
                return STATUS_EOF;
            if (shadow_pass)
                result = md5_crypt_verify(port->user_name, shadow_pass, passwd, md5Salt, md5Salt_len, logdetail);
            else
                result = STATUS_ERROR;
        */
        pfree(passwd);
    } else {
        PDEBUG("# 310: auth skip");
        discard_input();
    }
    ClientAuthInProgress = false;

    {
        StringInfoData buf;
        pq_beginmessage(&buf, 'R');
        pq_sendint32(&buf, (int32) AUTH_REQ_OK);
        pq_endmessage(&buf);
    }

    BeginReportingGUCOptions();
    pgstat_report_connect(MyDatabaseId);
    {
        StringInfoData buf;
        pq_beginmessage(&buf, 'K');
        pq_sendint32(&buf, (int32) MyProcPid);
        pq_sendint32(&buf, (int32) MyCancelKey);
        pq_endmessage(&buf);
    }
PDEBUG("# 330: TODO: set a pgl started flag");
    send_ready_for_query = true;
    ignore_till_sync = false;
    volatile int sf_connected = 0;
}

extern void pg_startcma();

__attribute__((export_name("interactive_one"))) void
interactive_one() {
    PGL_LOG_INFO("interactive_one: ENTRY - function called!");
    PGL_LOG_INFO("interactive_one: cma_rsize=%d cma_wsize=%d", cma_rsize, cma_wsize);
    PGL_LOG_INFO("interactive_one: is_wire=%d MyProcPort=%p", is_wire, (void*)MyProcPort);

    int	peek = -1;  /* preview of firstchar with no pos change */
	int firstchar = 0;  /* character read from getc() */
    bool pipelining = true;
	StringInfoData input_message;
	StringInfoData *inBuf;

#ifdef PGL_MOBILE
    PGL_LOG_INFO("interactive_one: ENTRY - backend is running and ready to process messages");
    PGL_LOG_INFO("interactive_one: MessageContext=%p", (void*)MessageContext);
#endif
    FILE *stream ;
    FILE *fp = NULL;
    int packetlen;
    bool repl_from_file = false; /* mobile: track REPL data source */

    bool had_notification = notifyInterruptPending;
    bool notified = false;
    // send_ready_for_query = false;
if (cma_rsize<0)
    goto resume_on_error;



    if (!MyProcPort) {
        PDEBUG("# 353: client created");
        PGL_LOG_INFO("interactive_one: calling io_init, port=%p", (void*)MyProcPort);
        io_init(is_wire, false);
        PGL_LOG_INFO("interactive_one: after io_init, port=%p sock=%d", (void*)MyProcPort, MyProcPort?MyProcPort->sock:-1);
    }

#ifdef PGL_MOBILE
    /* Mobile: Use CMA buffer directly like WASM, no sockets needed */
    PGL_LOG_INFO("interactive_one: Mobile section, is_wire=%d", is_wire);
    if (is_wire) {
        /* Ensure PostgreSQL has a valid port structure but no real socket */
        if (!MyProcPort) {
            MyProcPort = (Port *) calloc(1, sizeof(Port));
            if (MyProcPort) {
                MyProcPort->sock = -1; // No real socket, use CMA buffer directly
                PGL_LOG_INFO("created MyProcPort with no socket (CMA mode)");
            }
        }
        /* Mobile: Write CMA buffer to temporary file for PostgreSQL to read */
        if (cma_rsize > 0) {
            char *src = (char*)(intptr_t)get_buffer_addr(0);
            unsigned char first_byte = (unsigned char)src[0];
            PGL_LOG_INFO("CMA buffer ready: %d bytes, first_byte=0x%02x ('%c')",
                  cma_rsize, first_byte, first_byte >= 32 && first_byte < 127 ? first_byte : '?');

            /* Debug: log buffer content */
            PGL_LOG_INFO("CMA buffer content (first 20 bytes):");
            for (int i = 0; i < (cma_rsize < 20 ? cma_rsize : 20); i++) {
                PGL_LOG_INFO("  [%d] = 0x%02x ('%c')", i, (unsigned char)src[i],
                      src[i] >= 32 && src[i] < 127 ? src[i] : '?');
            }

            /* Reset CMA read offset for new request */
            // No need to reset - PQcommMethods handles buffer management

            /* Ensure mobile comm is installed before any auth messages are sent */
            #ifdef PGL_MOBILE
            extern void pgl_install_mobile_comm(void);
            whereToSendOutput = DestRemote; /* now safe to ereport to client */
            pgl_install_mobile_comm();
            #endif

            /* Kick off startup/auth once after first inbound data */
            if (!mobile_auth_started && !ClientAuthInProgress) {
                mobile_auth_started = true;
                PGL_LOG_INFO("CMA setup: invoking startup_auth");
                startup_auth();
                /* Mobile: flush immediately after auth to publish AuthenticationOk/ParameterStatus */
                pq_flush();
                extern volatile int cma_wsize;
                PGL_LOG_INFO("after startup_auth: cma_wsize=%d", cma_wsize);
                /* Mark startup message as consumed to prevent SocketBackend from re-reading it */
                PGL_LOG_INFO("Mobile: startup_auth complete, marking input buffer as consumed (cma_rsize %d -> 0)", cma_rsize);
                cma_rsize = 0;
            }
        }
        // PGL_LOG_INFO("wire setup: port=%p sock=%d wrote=%zd first_byte=0x%02x", (void*)MyProcPort, MyProcPort?MyProcPort->sock:-1, wrote, first_byte);
    }
#endif

#if PGDEBUG
    if (notifyInterruptPending)
        PDEBUG("# 371: has notification !");
#endif

    // this could be pg_flush in sync mode.
    // but in fact we are writing socket data that was piled up previous frame async.
    if (SOCKET_DATA>0) {
        puts("# 361: ERROR flush after frame");
        goto wire_flush;
    }

    if (!cma_rsize) {
        // no CMA input
#ifndef PGL_MOBILE
        if (!SOCKET_FILE) {
            SOCKET_FILE = fopen(PGS_OLOCK, "w");
            if (SOCKET_FILE && MyProcPort) MyProcPort->sock = fileno(SOCKET_FILE);
            else if (MyProcPort) MyProcPort->sock = -1;
        }
#else
        /* Mobile: CMA-only; do not open socket files */
#endif
    } else {
#ifndef PGL_MOBILE
        // prepare file reply queue, just in case of overflow
        if (!SOCKET_FILE) {
            SOCKET_FILE = fopen(PGS_OLOCK, "w");
        }
#else
        /* Mobile: CMA-only; no socket file reply queue */
#endif
    }

    /* Defensive: ensure MessageContext exists (mobile may re-enter without setup) */
    if (MessageContext == NULL) {
        MessageContext = AllocSetContextCreate(TopMemoryContext,
                                              "MessageContext",
                                              ALLOCSET_DEFAULT_SIZES);
    }
    MemoryContextSwitchTo(MessageContext);
    MemoryContextReset(MessageContext);

    initStringInfo(&input_message);

    inBuf = &input_message;

	InvalidateCatalogSnapshotConditionally();

	if (send_ready_for_query) {

		if (IsAbortedTransactionBlockState()) {
			PDEBUG("@@@@ TODO 403: idle in transaction (aborted)");
		}
		else if (IsTransactionOrTransactionBlock()) {
			PDEBUG("@@@@ TODO 406: idle in transaction");
		} else {
			if (notifyInterruptPending) {
				ProcessNotifyInterrupt(false);
                notified = true;
            }
        }
        send_ready_for_query = false;
    }
// postgres.c 4627
    DoingCommandRead = true;

#if defined(__ANDROID__) || defined(__APPLE__)
    /* On mobile, avoid dereferencing address 1; use empty immutable buffer */
    #undef IO
    #define IO ((char *)"")
    /* Access CMA buffer address for peeking the first byte */
    #include "../mobile-build/sdk_port-mobile.h"
#endif

#if defined(EMUL_CMA)
    //  temp fix for -O0 but less efficient than literal
    #define IO ((char *)(1+(int)cma_port))
#else
  #if defined(__ANDROID__) || defined(__APPLE__)
    // Mobile: do not reference CMA via IO; IO is unused on mobile. PQcomm reads CMA directly.
    #define IO ((char *)"")
  #else
    #define IO ((char *)(1))
  #endif
#endif

/*
 * in cma mode (cma_rsize>0), client call the wire loop itself waiting synchronously for the results
 * in socketfiles mode, the wire loop polls a pseudo socket made from incoming and outgoing files.
 * in repl mode (cma_rsize==0) output is on stdout not cma/socketfiles wire.
 * repl mode is the simpleset mode where stdin is just copied into input buffer (limited by CMA size).
 * TODO: allow to redirect stdout for fully external repl.
 */

#ifdef PGL_MOBILE
    /* On mobile, peek directly from CMA input buffer to set firstchar correctly */
    peek = ((const unsigned char*)(intptr_t)get_buffer_addr(0))[0];
#else
    peek = IO[0];
#endif
    packetlen = cma_rsize;


#if PGDEBUG && !defined(PGL_MOBILE)
    /* Debug: log what PostgreSQL reads from IO buffer vs socket */
    if (packetlen > 0) {
        PGL_LOG_INFO("PostgreSQL reads from IO[0]: peek=0x%02x ('%c'), packetlen=%d",
              (unsigned char)peek, peek >= 32 && peek < 127 ? peek : '?', packetlen);
        PGL_LOG_INFO("IO buffer content (first 10 bytes):");
        for (int i = 0; i < (packetlen < 10 ? packetlen : 10); i++) {
            PGL_LOG_INFO("  IO[%d] = 0x%02x ('%c')", i, (unsigned char)IO[i],
                  IO[i] >= 32 && IO[i] < 127 ? IO[i] : '?');
        }
    }
#endif

    if (packetlen) {
        sockfiles = false;
        if (!is_repl) {
            whereToSendOutput = DestRemote;
            if (!is_wire)
                PDEBUG("# 439: repl message in cma buffer !");
        } else {
            if (is_wire)
                PDEBUG("# 442: wire message in cma buffer for REPL !");
            whereToSendOutput = DestDebug;
        }
    } else {
#ifndef PGL_MOBILE
        fp = fopen(PGS_IN, "r");
        if (fp) {
            fseek(fp, 0L, SEEK_END);
            packetlen = ftell(fp);
            if (packetlen) {
                resetStringInfo(inBuf);
                rewind(fp);
                /* peek on first char */
                peek = getc(fp);
                rewind(fp);
                if (is_repl && !is_wire) {
                    for (int i=0; i<packetlen; i++) {
                        appendStringInfoChar(inBuf, fgetc(fp));
                    }
                    sockfiles = false;
                    repl_from_file = true;
                } else {
                    whereToSendOutput = DestRemote;
                    pq_recvbuf_fill(fp, packetlen);
                    sockfiles = true;
                }
                if (!peek) { startup_auth(); peek = -1; }
                if (peek==112) { startup_pass(true); peek = -1; }
            }
            if (packetlen) {
                if (peek<0) { goto wire_flush; }
                firstchar = peek;
                goto incoming;
            }
        }
#else
        /* Mobile: CMA-only; no file input */
#endif

        // is it REPL in cma ?
#ifndef PGL_MOBILE
        if (!peek)
            goto return_early;

        firstchar = peek ;

        // REPL mode in zero copy buffer (lowest wasm memory segment)
        packetlen = strlen(IO);
#else
        /* Mobile: CMA-only; skip REPL path entirely */
        goto return_early;
#endif

    } // !cma_rsize -> socketfiles -> repl

#if PGDEBUG
#ifndef PGL_MOBILE
    if (packetlen)
        IO[packetlen]=0; // wire blocks are not zero terminated
#endif
    printf("\n# 524: fd=%d is_embed=%d is_repl=%d is_wire=%d fd %s,len=%d cma=%d peek=%d [%s]\n", MyProcPort->sock, is_embed, is_repl, is_wire, PGS_OLOCK, packetlen,cma_rsize, peek, IO);
#endif

    if (!repl_from_file) {
        resetStringInfo(inBuf);
    }
#ifndef PGL_MOBILE
    // when cma buffer is used to fake stdin, data is not read by socket/wire backend.
    if (is_repl && !repl_from_file) {
        for (int i=0; i<packetlen; i++) {
            appendStringInfoChar(inBuf, IO[i]);
        }
    }
#endif

    if (packetlen<2) {
        puts("# 536: WARNING: empty packet");
        //cma_rsize= 0;
        if (is_repl)
            pg_prompt();
        // always free cma buffer !!!
        // IO[0] = 0;
        goto return_early;
    }

incoming:
#if defined(__EMSCRIPTEN__) || defined(__wasi__) || defined(PGL_MOBILE)
#   include "pgl_sjlj.c"
#else
    #error "sigsetjmp unsupported"
#endif


    while (pipelining) {

        if (is_repl) {
            // are we sure repl could not pipeline ?
            pipelining = false;
            /* stdio node repl */
#if PGDEBUG
            printf("\n# 533: enforcing REPL mode, wire off, echo %d\n", force_echo);

#endif
            whereToSendOutput = DestDebug;
        }

        DoingCommandRead = true;
        /* Install mobile comm methods if present */
#ifdef PGL_MOBILE
        extern void pgl_install_mobile_comm(void);
        pgl_install_mobile_comm();
#endif

        if (is_wire) {
            /* wire on a socket or cma may auth */
            /* would be handled as error by pg_proto block */
#ifdef PGL_MOBILE
            if (peek==0 && !mobile_auth_started) {
#else
            if (peek==0) {
#endif
                PDEBUG("# 540: handshake/auth");
                startup_auth();
                PDEBUG("# 542: auth request");
                /* Mark startup message as consumed */
                PGL_LOG_INFO("Mobile: startup_auth complete, marking input buffer as consumed (cma_rsize %d -> 0)", cma_rsize);
                cma_rsize = 0;
                break;
            }

#ifdef PGL_MOBILE
            /* Mobile: flush output but continue processing batch until complete */
            pq_flush();
            if (cma_wsize > 0) {
                channel = 1;
                // mobile_log("flush(after SocketBackend): published %d bytes to CMA", cma_wsize);
            }
            /* CMA mode: output is already in CMA buffer via PQcommMethods */
            PGL_LOG_INFO("drain(after SocketBackend): CMA mode, cma_wsize=%d", cma_wsize);
#endif

            if (peek==112) {
                PDEBUG("# 547: password");
                startup_pass(true);
                /* Mark password message as consumed */
                PGL_LOG_INFO("Mobile: startup_pass complete, marking input buffer as consumed (cma_rsize %d -> 0)", cma_rsize);
                cma_rsize = 0;
                break;
            }

            // PGL_LOG_INFO("interactive_one: before SocketBackend, port=%p sock=%d", (void*)MyProcPort, MyProcPort?MyProcPort->sock:-1);
            // PGL_LOG_INFO("interactive_one: cma_rsize=%d peek=%d", cma_rsize, peek);

            /* CMA mode: no socket polling needed */

            /* Mobile: Reset CMA read offset and use normal PostgreSQL flow */
            // No need to reset - PQcommMethods handles buffer management
            PGL_LOG_INFO("Mobile CMA mode: reset read offset, using SocketBackend");

            /* Let SocketBackend handle message reading, pq_getbyte will read from CMA buffer */
            firstchar = SocketBackend(inBuf);
            PGL_LOG_INFO("Mobile: SocketBackend returned firstchar=%d ('%c') inBuf->len=%d",
                  firstchar, firstchar > 0 && firstchar < 127 ? firstchar : '?', inBuf->len);

            /* Debug: log what SocketBackend read */
            if (inBuf->len > 0) {
                PGL_LOG_INFO("SocketBackend read (first 20 bytes):");
                for (int i = 0; i < (inBuf->len < 20 ? inBuf->len : 20); i++) {
                    PGL_LOG_INFO("  inBuf[%d] = 0x%02x ('%c')", i, (unsigned char)inBuf->data[i],
                          inBuf->data[i] >= 32 && inBuf->data[i] < 127 ? inBuf->data[i] : '?');
                }
            }

            /* Don't reset cma_rsize yet - let pipelining check handle remaining messages */

            pipelining = pq_buffer_remaining_data()>0;
        } else {
            /* nowire */
            // pipelining = false;
            if (firstchar == EOF && inBuf->len == 0) {
                firstchar = EOF;
            } else {
                appendStringInfoChar(inBuf, (char) '\0');
            	firstchar = 'Q';
            }
#ifdef PGL_MOBILE
            /* --- Mobile CMA mode: data is already in buffer --- */
            PGL_LOG_INFO("Mobile CMA: data ready in buffer, cma_rsize=%d", cma_rsize);
            /* --------------------------------------------------------------- */
#endif

        }
        DoingCommandRead = false;
#ifdef PGL_MOBILE
            // Drain server replies from client end into CMA out
            if (MyProcPort && MyProcPort->sock > 0) {
                // We wrote into sv[1]; recover its fd by duplicating MyProcPort->sock?
                // We stored both ends in local sv[], so add a static to retain them across scope
            }
#endif


        if (!ignore_till_sync) {
            /* initially, or after error */
            // send_ready_for_query = true;
            if (notifyInterruptPending)
               ProcessClientReadInterrupt(true);
        } else {
            /* ignoring till sync will skip all pipeline */
            if (firstchar != EOF) {
                if (firstchar != 'S') {
                    continue;
                }
            }
        }

        #include "pg_proto.c"

        if (pipelining) {
            pipelining = pq_buffer_remaining_data()>0;
            if (pipelining && send_ready_for_query) {
                ReadyForQuery(whereToSendOutput);
                send_ready_for_query = false;
            }
        }
    }
#ifdef PGL_MOBILE
    /* Mobile: Ensure all output is flushed before marking batch as consumed */
    if (cma_rsize > 0) {
        /* Force a final flush to make sure all accumulated output is visible */
        pq_flush();
        PGL_LOG_INFO("Mobile: Pipelining complete, final flush done, cma_wsize=%d", cma_wsize);
        PGL_LOG_INFO("Mobile: Pipelining complete, marking batch as consumed (cma_rsize %d -> 0)", cma_rsize);
        cma_rsize = 0;
    }
#endif
resume_on_error:
    if (!is_repl) {
wire_flush:
        if (!ClientAuthInProgress) {
            /* process notifications (SYNC) */
            if (notifyInterruptPending)
               ProcessNotifyInterrupt(false);

            if (send_ready_for_query) {
//                PDEBUG("# 602: end packet - sending rfq\n");
                ReadyForQuery(DestRemote);
                //done at postgres.c 4623
                send_ready_for_query = false;
            } else {
                PDEBUG("# 606: end packet - with no rfq\n");
            }
#ifdef PGL_MOBILE
            /* Mobile: flush output but let pipelining loop handle completion */
            pq_flush();
            if (cma_wsize > 0) {
                channel = 1;
                // mobile_log("flush(wire_flush): published %d bytes to CMA", cma_wsize);
            } else {
                // mobile_log("flush(wire_flush): no pending bytes");
            }
#endif
        } else {
            PDEBUG("# 609: end packet (ClientAuthInProgress - no rfq)\n");
        }

        if (SOCKET_DATA>0) {
#ifdef PGL_MOBILE
        if (cma_wsize > 0) {
            /* Mobile PqComm already produced CMA output. Skip socket fallback. */
        } else
#endif

#ifndef PGL_MOBILE
            if (sockfiles) {
                channel = -1;
                if (cma_wsize) {
                    puts("ERROR: cma was not flushed before socketfile interface");
                }
            } else {
                cma_wsize = SOCKET_DATA;
                channel = cma_rsize + 2;
            }
            if (SOCKET_FILE) {
                int outb = SOCKET_DATA;
                fclose(SOCKET_FILE);
                SOCKET_FILE = NULL;
                SOCKET_DATA = 0;
#if PGDEBUG
                if (cma_wsize) {
                    PDEBUG("# 672: cma and sockfile ???\n");
                }
                if (sockfiles) {
                    printf("# 675: client:ready -> read(%d) " PGS_OLOCK "->" PGS_OUT"\n", outb);
                }
#endif
                if (sockfiles) {
                    rename(PGS_OLOCK, PGS_OUT);
                }
            }
#else
            /* Mobile: CMA-only; no socket fallback */
#endif
            {
#if PGDEBUG
#ifdef PGL_MOBILE
            if (cma_wsize == 0)
#endif

                printf("\n# 681: in[%d] out[%d] flushed\n", cma_rsize, cma_wsize);
#endif
                SOCKET_DATA = 0;
            }

        } else {
#ifndef PGL_MOBILE
            cma_wsize = 0;
#else
            /* Mobile: do not clear cma_wsize here. If PQcommMethods flushed output,
               RN will read it after this function returns. */
#endif
            PDEBUG("# 698: no data, send empty ?");
// TODO: dedup 739
#ifndef PGL_MOBILE
            if (sockfiles) {
                if (SOCKET_FILE) { fclose(SOCKET_FILE); SOCKET_FILE = NULL; }
                rename(PGS_OLOCK, PGS_OUT);
            }
#endif
        }
    } else {
        pg_prompt();
#if PGDEBUG
        puts("# 683: repl output");
        if (SOCKET_DATA>0) {
                puts("# 686: socket has data");
            if (sockfiles)
                printf("# 688: socket file not flushed -> read(%d) " PGS_OLOCK "->" PGS_OUT"\n", SOCKET_DATA);
        } else {
// TODO: dedup 723
            if (sockfiles) {
                if (SOCKET_FILE) { fclose(SOCKET_FILE); SOCKET_FILE = NULL; }
                rename(PGS_OLOCK, PGS_OUT);
            }
        }
        if (cma_wsize)
            puts("ERROR: cma was not flushed before socketfile interface");
#endif
    }
return_early:;
    /* always FD CLEANUP */
    if (fp) {
        fclose(fp);
#ifndef PGL_MOBILE
        unlink(PGS_IN);
#endif
    }


    // always free kernel buffer !!!
    cma_rsize = 0;
#ifndef PGL_MOBILE
    IO[0] = 0;
#endif

#ifdef PGL_MOBILE
    /* CMA mode: no cleanup needed, just reset buffer */
#endif

    #undef IO

    // reset EX counter
    canary_ex = 0;
}

#undef PGL_LOOP
