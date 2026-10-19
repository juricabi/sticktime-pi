/* Platform layer: time, threads, files and sockets.
 * The device is Linux (static aarch64). Most of the host also builds for Windows
 * (zig cc / mingw) so the game, the drawing and the web server can be tested on a PC. */
#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET plat_sock;
#define PLAT_BADSOCK INVALID_SOCKET
typedef int plat_socklen;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
typedef int plat_sock;
#define PLAT_BADSOCK (-1)
typedef socklen_t plat_socklen;
#endif

/* ---------------------------------------------------------------- time */
int64_t plat_now_us(void);                 /* monotonic clock, microseconds */
int64_t plat_wall_ms(void);                /* wall clock, ms since 1970 */
void plat_sleep_us(int64_t us);            /* relative; returns early only on error */
void plat_sleep_until_us(int64_t t_us);    /* absolute, on the plat_now_us() clock */

/* ---------------------------------------------------------------- threads */
#ifdef _WIN32
typedef HANDLE plat_thread;
typedef CRITICAL_SECTION plat_mutex;
#else
typedef pthread_t plat_thread;
typedef pthread_mutex_t plat_mutex;
#endif
#ifdef _WIN32
typedef CONDITION_VARIABLE plat_cond;
#else
typedef pthread_cond_t plat_cond;   /* waits use CLOCK_MONOTONIC */
#endif
int  plat_thread_start(plat_thread *t, void *(*fn)(void *), void *arg, bool detached);
int  plat_thread_join(plat_thread t);
void plat_thread_lower_priority(void);     /* calling thread: below the frame loop, on any core */
/* the CPUs online (the highest number + 1); not sysconf(_SC_NPROCESSORS_ONLN): musl counts only the
 * calling thread's affinity mask, so a frame loop pinned to one core would see a single CPU */
int  plat_cpus_online(void);
void plat_mutex_init(plat_mutex *m);
void plat_mutex_lock(plat_mutex *m);
void plat_mutex_unlock(plat_mutex *m);
void plat_mutex_destroy(plat_mutex *m);
void plat_cond_init(plat_cond *c);
void plat_cond_signal(plat_cond *c);
void plat_cond_broadcast(plat_cond *c);
/* wait with m locked; returns 0 when signalled (or spuriously), 1 on timeout. ms < 0 = forever */
int  plat_cond_wait(plat_cond *c, plat_mutex *m, int ms);

/* ---------------------------------------------------------------- files
 * Paths use '/' on every platform. Errors return -1 (or NULL) and leave errno set. */
typedef struct {
  bool exists, is_dir;
  int64_t size;
  int64_t mtime;                           /* seconds since 1970 */
} plat_stat_t;

int   plat_stat(const char *path, plat_stat_t *st);   /* 0 ok (st->exists false if missing), -1 other error */
char *plat_read_file(const char *path, size_t *len, size_t max);  /* malloc'd, NUL-terminated; NULL if missing/too big */
int   plat_write_file_atomic(const char *path, const void *data, size_t len); /* temp + fsync + rename */
int   plat_mkdirs(const char *path);       /* mkdir -p; 0 if it exists */
int   plat_remove(const char *path);       /* a file or an empty directory */
int   plat_rename(const char *from, const char *to);  /* replaces an existing file */
/* calls cb for each entry except . and ..; stops when cb returns nonzero (returned) */
typedef int (*plat_dir_cb)(void *ud, const char *name, bool is_dir, int64_t size, int64_t mtime);
int   plat_list_dir(const char *path, plat_dir_cb cb, void *ud);
void  plat_sync_dir(const char *dir);      /* fsync a directory (Linux), no-op elsewhere */

/* ---------------------------------------------------------------- sockets */
int  plat_net_init(void);                  /* WSAStartup on Windows; ignores SIGPIPE on Linux */
void plat_sock_close(plat_sock s);
int  plat_sock_nonblock(plat_sock s, bool on);
int  plat_sock_timeouts(plat_sock s, int recv_ms, int send_ms);
int  plat_sock_errno(void);                /* last socket error */
bool plat_sock_would_block(void);          /* last error was EAGAIN/EWOULDBLOCK/EINPROGRESS */
/* UDP socket bound to 0.0.0.0:port, non-blocking, SO_RCVBUF 256 KB; optionally SO_BROADCAST (to send
 * broadcasts) and SO_REUSEADDR (share the port: off for the joystick port, so "port in use" is detected;
 * Windows then uses SO_EXCLUSIVEADDRUSE) */
plat_sock plat_udp_bind(uint16_t port, bool broadcast, bool reuse);
/* TCP listening socket on addr (NULL = any):port, SO_REUSEADDR */
plat_sock plat_tcp_listen(const char *addr, uint16_t port, int backlog);
/* blocking TCP connect with a timeout; returns a blocking socket or PLAT_BADSOCK */
plat_sock plat_tcp_connect(const char *ip, uint16_t port, int timeout_ms);

/* ---------------------------------------------------------------- misc */
size_t plat_strlcpy(char *dst, const char *src, size_t size);
/* run a command line in the background (sh -c on Linux); returns 0 if it was started */
int plat_spawn(const char *cmdline);
/* run a command and capture up to max-1 bytes of its stdout; returns exit status or -1.
 * Blocks: call it from a worker thread, never from the frame loop. */
int plat_run_capture(const char *cmdline, char *out, size_t max, int timeout_ms);

#endif
