/* Platform layer: see plat.h. Linux is the target; Windows is for tests on a PC. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "plat.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <timeapi.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ================================================================ time */
#ifdef _WIN32
static LARGE_INTEGER qpf;
int64_t plat_now_us(void) {
  LARGE_INTEGER c;
  if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
  QueryPerformanceCounter(&c);
  return (int64_t)((double)c.QuadPart * 1e6 / (double)qpf.QuadPart);
}
int64_t plat_wall_ms(void) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;   /* 100 ns since 1601 */
  return (int64_t)(t / 10000 - 11644473600000ULL);
}
void plat_sleep_us(int64_t us) {
  if (us <= 0) return;
  /* Sleep() has ~1 ms granularity with timeBeginPeriod; spin the last bit for tests */
  static bool period_set;
  if (!period_set) { timeBeginPeriod(1); period_set = true; }
  int64_t end = plat_now_us() + us;
  if (us > 2000) Sleep((DWORD)((us - 1500) / 1000));
  while (plat_now_us() < end) Sleep(0);
}
void plat_sleep_until_us(int64_t t) { plat_sleep_us(t - plat_now_us()); }
#else
int64_t plat_now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
int64_t plat_wall_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void plat_sleep_us(int64_t us) {
  if (us <= 0) return;
  plat_sleep_until_us(plat_now_us() + us);
}
void plat_sleep_until_us(int64_t t) {
  struct timespec ts = { (time_t)(t / 1000000), (long)(t % 1000000) * 1000 };
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}
#endif

/* ================================================================ threads */
#ifdef _WIN32
typedef struct { void *(*fn)(void *); void *arg; } thunk_t;
static DWORD WINAPI thunk(LPVOID p) {
  thunk_t t = *(thunk_t *)p;
  free(p);
  t.fn(t.arg);
  return 0;
}
int plat_thread_start(plat_thread *t, void *(*fn)(void *), void *arg, bool detached) {
  thunk_t *k = malloc(sizeof *k);
  if (!k) return -1;
  k->fn = fn; k->arg = arg;
  HANDLE h = CreateThread(NULL, 0, thunk, k, 0, NULL);
  if (!h) { free(k); return -1; }
  if (detached) { CloseHandle(h); h = NULL; }
  if (t) *t = h;
  return 0;
}
int plat_thread_join(plat_thread t) {
  if (!t) return -1;
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
  return 0;
}
void plat_thread_lower_priority(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
void plat_mutex_init(plat_mutex *m) { InitializeCriticalSection(m); }
void plat_mutex_lock(plat_mutex *m) { EnterCriticalSection(m); }
void plat_mutex_unlock(plat_mutex *m) { LeaveCriticalSection(m); }
void plat_mutex_destroy(plat_mutex *m) { DeleteCriticalSection(m); }
void plat_cond_init(plat_cond *c) { InitializeConditionVariable(c); }
void plat_cond_signal(plat_cond *c) { WakeConditionVariable(c); }
void plat_cond_broadcast(plat_cond *c) { WakeAllConditionVariable(c); }
int plat_cond_wait(plat_cond *c, plat_mutex *m, int ms) {
  if (SleepConditionVariableCS(c, m, ms < 0 ? INFINITE : (DWORD)ms)) return 0;
  return GetLastError() == ERROR_TIMEOUT ? 1 : 0;
}
#else
int plat_thread_start(plat_thread *t, void *(*fn)(void *), void *arg, bool detached) {
  pthread_t th;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 * 1024);
  if (detached) pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&th, &a, fn, arg);
  pthread_attr_destroy(&a);
  if (r) { errno = r; return -1; }
  if (t) *t = th;
  return 0;
}
int plat_thread_join(plat_thread t) { return pthread_join(t, NULL) ? -1 : 0; }
void plat_thread_lower_priority(void) {
  /* worker threads never run real-time, whatever the process was started with, nor only on the frame
   * loop's core: a thread inherits its creator's policy and CPU mask */
  struct sched_param sp = { 0 };
  pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
#ifdef __linux__
  /* every bit: the kernel keeps the CPUs that exist and the process may use (musl's sysconf counts
   * the CPUs of the caller's mask, which is the one inherited pin) */
  cpu_set_t all;
  CPU_ZERO(&all);
  for (int i = 0; i < CPU_SETSIZE; i++) CPU_SET(i, &all);
  pthread_setaffinity_np(pthread_self(), sizeof all, &all);
#endif
}
void plat_mutex_init(plat_mutex *m) { pthread_mutex_init(m, NULL); }
void plat_mutex_lock(plat_mutex *m) { pthread_mutex_lock(m); }
void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(m); }
void plat_mutex_destroy(plat_mutex *m) { pthread_mutex_destroy(m); }
void plat_cond_init(plat_cond *c) {
  pthread_condattr_t a;
  pthread_condattr_init(&a);
  pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  pthread_cond_init(c, &a);
  pthread_condattr_destroy(&a);
}
void plat_cond_signal(plat_cond *c) { pthread_cond_signal(c); }
void plat_cond_broadcast(plat_cond *c) { pthread_cond_broadcast(c); }
int plat_cond_wait(plat_cond *c, plat_mutex *m, int ms) {
  if (ms < 0) { pthread_cond_wait(c, m); return 0; }
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  return pthread_cond_timedwait(c, m, &ts) == ETIMEDOUT ? 1 : 0;
}
#endif

/* ================================================================ files */
#ifdef _WIN32
/* UTF-8 paths with '/' -> wide paths */
static wchar_t *wpath(const char *p) {
  int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
  if (!w) return NULL;
  MultiByteToWideChar(CP_UTF8, 0, p, -1, w, n);
  for (wchar_t *c = w; *c; c++) if (*c == L'/') *c = L'\\';
  return w;
}
static int64_t ft_secs(FILETIME ft) {
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (int64_t)(t / 10000000ULL) - 11644473600LL;
}
static void set_errno_from_win(void) {
  DWORD e = GetLastError();
  errno = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT
        : (e == ERROR_ACCESS_DENIED) ? EACCES
        : (e == ERROR_DIR_NOT_EMPTY) ? ENOTEMPTY
        : (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) ? EEXIST : EIO;
}
int plat_stat(const char *path, plat_stat_t *st) {
  memset(st, 0, sizeof *st);
  wchar_t *w = wpath(path);
  if (!w) return -1;
  WIN32_FILE_ATTRIBUTE_DATA d;
  BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
  free(w);
  if (!ok) {
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME) return 0;
    set_errno_from_win();
    return -1;
  }
  st->exists = true;
  st->is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  st->size = st->is_dir ? 0 : (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
  st->mtime = ft_secs(d.ftLastWriteTime);
  return 0;
}
static FILE *open_file(const char *path, const char *mode) {
  wchar_t *w = wpath(path);
  if (!w) return NULL;
  wchar_t wm[8];
  size_t i = 0;
  for (; mode[i] && i < 7; i++) wm[i] = (wchar_t)mode[i];
  wm[i] = 0;
  FILE *f = _wfopen(w, wm);
  free(w);
  return f;
}
int plat_mkdirs(const char *path) {
  char buf[1024];
  if (plat_strlcpy(buf, path, sizeof buf) >= sizeof buf) { errno = ENAMETOOLONG; return -1; }
  for (char *p = buf + 1; ; p++) {
    if (*p == '/' || *p == 0) {
      char c = *p;
      *p = 0;
      if (!(p - buf == 2 && buf[1] == ':')) {      /* skip "C:" */
        wchar_t *w = wpath(buf);
        if (!w) return -1;
        if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) { set_errno_from_win(); free(w); return -1; }
        free(w);
      }
      *p = c;
      if (!c) break;
    }
  }
  return 0;
}
int plat_remove(const char *path) {
  wchar_t *w = wpath(path);
  if (!w) return -1;
  DWORD a = GetFileAttributesW(w);
  BOOL ok = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? RemoveDirectoryW(w) : DeleteFileW(w);
  if (!ok) set_errno_from_win();
  free(w);
  return ok ? 0 : -1;
}
int plat_rename(const char *from, const char *to) {
  wchar_t *a = wpath(from), *b = wpath(to);
  BOOL ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) set_errno_from_win();
  free(a); free(b);
  return ok ? 0 : -1;
}
int plat_list_dir(const char *path, plat_dir_cb cb, void *ud) {
  char pat[1024];
  snprintf(pat, sizeof pat, "%s/*", path);
  wchar_t *w = wpath(pat);
  if (!w) return -1;
  WIN32_FIND_DATAW d;
  HANDLE h = FindFirstFileW(w, &d);
  free(w);
  if (h == INVALID_HANDLE_VALUE) { set_errno_from_win(); return -1; }
  int r = 0;
  do {
    if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
    char name[520];
    if (!WideCharToMultiByte(CP_UTF8, 0, d.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
    bool dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    int64_t size = dir ? 0 : (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    r = cb(ud, name, dir, size, ft_secs(d.ftLastWriteTime));
  } while (!r && FindNextFileW(h, &d));
  FindClose(h);
  return r;
}
void plat_sync_dir(const char *dir) { (void)dir; }
static int sync_file(FILE *f) { return _commit(_fileno(f)); }
#else
int plat_stat(const char *path, plat_stat_t *st) {
  memset(st, 0, sizeof *st);
  struct stat s;
  if (stat(path, &s) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) return 0;
    return -1;
  }
  st->exists = true;
  st->is_dir = S_ISDIR(s.st_mode);
  st->size = st->is_dir ? 0 : (int64_t)s.st_size;
  st->mtime = (int64_t)s.st_mtime;
  return 0;
}
static FILE *open_file(const char *path, const char *mode) { return fopen(path, mode); }
int plat_mkdirs(const char *path) {
  char buf[1024];
  if (plat_strlcpy(buf, path, sizeof buf) >= sizeof buf) { errno = ENAMETOOLONG; return -1; }
  for (char *p = buf + 1; ; p++) {
    if (*p == '/' || *p == 0) {
      char c = *p;
      *p = 0;
      if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
      *p = c;
      if (!c) break;
    }
  }
  return 0;
}
int plat_remove(const char *path) {
  struct stat s;
  if (stat(path, &s) == 0 && S_ISDIR(s.st_mode)) return rmdir(path);
  return unlink(path);
}
int plat_rename(const char *from, const char *to) { return rename(from, to); }
int plat_list_dir(const char *path, plat_dir_cb cb, void *ud) {
  DIR *d = opendir(path);
  if (!d) return -1;
  struct dirent *e;
  int r = 0;
  char full[1200];
  while (!r && (e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(full, sizeof full, "%s/%s", path, e->d_name);
    struct stat s;
    if (stat(full, &s) != 0) continue;
    r = cb(ud, e->d_name, S_ISDIR(s.st_mode), S_ISDIR(s.st_mode) ? 0 : (int64_t)s.st_size, (int64_t)s.st_mtime);
  }
  closedir(d);
  return r;
}
void plat_sync_dir(const char *dir) {
  int fd = open(dir, O_RDONLY | O_DIRECTORY);
  if (fd >= 0) { fsync(fd); close(fd); }
}
static int sync_file(FILE *f) { return fsync(fileno(f)); }
#endif

char *plat_read_file(const char *path, size_t *len, size_t max) {
  FILE *f = open_file(path, "rb");
  if (!f) return NULL;
  size_t cap = 4096, n = 0;
  char *buf = malloc(cap + 1);
  if (!buf) { fclose(f); return NULL; }
  for (;;) {
    if (n == cap) {
      if (cap >= max) { free(buf); fclose(f); errno = EFBIG; return NULL; }
      cap = cap * 2 > max ? max : cap * 2;
      char *nb = realloc(buf, cap + 1);
      if (!nb) { free(buf); fclose(f); errno = ENOMEM; return NULL; }
      buf = nb;
    }
    size_t r = fread(buf + n, 1, cap - n, f);
    if (r == 0) break;
    n += r;
  }
  int err = ferror(f);
  fclose(f);
  if (err) { free(buf); errno = EIO; return NULL; }
  buf[n] = 0;
  if (len) *len = n;
  return buf;
}

int plat_write_file_atomic(const char *path, const void *data, size_t len) {
  char tmp[1100];
  if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp~", path) >= sizeof tmp) { errno = ENAMETOOLONG; return -1; }
  FILE *f = open_file(tmp, "wb");
  if (!f) return -1;
  int ok = (len == 0 || fwrite(data, 1, len, f) == len) && fflush(f) == 0 && sync_file(f) == 0;
  int e = errno;
  if (fclose(f) != 0) ok = 0;
  if (!ok) { plat_remove(tmp); errno = e ? e : EIO; return -1; }
  if (plat_rename(tmp, path) != 0) { e = errno; plat_remove(tmp); errno = e; return -1; }
  /* make the rename itself durable */
  char dir[1100];
  plat_strlcpy(dir, path, sizeof dir);
  char *slash = strrchr(dir, '/');
  if (slash) { *slash = 0; plat_sync_dir(dir[0] ? dir : "/"); }
  else plat_sync_dir(".");
  return 0;
}

/* ================================================================ sockets */
#ifdef _WIN32
int plat_net_init(void) {
  WSADATA w;
  return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
}
void plat_sock_close(plat_sock s) { if (s != PLAT_BADSOCK) closesocket(s); }
int plat_sock_nonblock(plat_sock s, bool on) { u_long v = on; return ioctlsocket(s, FIONBIO, &v) == 0 ? 0 : -1; }
int plat_sock_timeouts(plat_sock s, int recv_ms, int send_ms) {
  DWORD r = (DWORD)recv_ms, w = (DWORD)send_ms;
  int a = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&r, sizeof r);
  int b = setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&w, sizeof w);
  return a == 0 && b == 0 ? 0 : -1;
}
int plat_sock_errno(void) { return WSAGetLastError(); }
bool plat_sock_would_block(void) {
  int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAETIMEDOUT;
}
#else
int plat_net_init(void) {
  signal(SIGPIPE, SIG_IGN);
  return 0;
}
void plat_sock_close(plat_sock s) { if (s >= 0) close(s); }
int plat_sock_nonblock(plat_sock s, bool on) {
  int fl = fcntl(s, F_GETFL, 0);
  if (fl < 0) return -1;
  return fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}
int plat_sock_timeouts(plat_sock s, int recv_ms, int send_ms) {
  struct timeval r = { recv_ms / 1000, (recv_ms % 1000) * 1000 }, w = { send_ms / 1000, (send_ms % 1000) * 1000 };
  int a = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof r);
  int b = setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &w, sizeof w);
  return a == 0 && b == 0 ? 0 : -1;
}
int plat_sock_errno(void) { return errno; }
bool plat_sock_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }
#endif

plat_sock plat_udp_bind(uint16_t port, bool broadcast, bool reuse) {
  plat_sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == PLAT_BADSOCK) return s;
  int one = 1;
  if (reuse) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#ifdef _WIN32
  else setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#endif
  if (broadcast) setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
  int rcv = 256 * 1024;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof rcv);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || plat_sock_nonblock(s, true) != 0) {
    int e = plat_sock_errno();
    plat_sock_close(s);
#ifdef _WIN32
    WSASetLastError(e);           /* closesocket() resets it */
#else
    errno = e;
#endif
    return PLAT_BADSOCK;
  }
  return s;
}

plat_sock plat_tcp_listen(const char *addr, uint16_t port, int backlog) {
  plat_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == PLAT_BADSOCK) return s;
  int one = 1;
#ifdef _WIN32
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#endif
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (addr && *addr && inet_pton(AF_INET, addr, &a.sin_addr) != 1) { plat_sock_close(s); return PLAT_BADSOCK; }
  if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, backlog) != 0) {
    plat_sock_close(s);
    return PLAT_BADSOCK;
  }
  return s;
}

plat_sock plat_tcp_connect(const char *ip, uint16_t port, int timeout_ms) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return PLAT_BADSOCK;
  plat_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == PLAT_BADSOCK) return s;
  plat_sock_nonblock(s, true);
  int r = connect(s, (struct sockaddr *)&a, sizeof a);
  if (r != 0 && !plat_sock_would_block()) { plat_sock_close(s); return PLAT_BADSOCK; }
  if (r != 0) {
    fd_set w, e;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(s, &w); FD_SET(s, &e);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    r = select((int)s + 1, NULL, &w, &e, &tv);
    int err = 0;
    plat_socklen len = sizeof err;
    if (r <= 0 || FD_ISSET(s, &e) || getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0 || err) {
      plat_sock_close(s);
      return PLAT_BADSOCK;
    }
  }
  plat_sock_nonblock(s, false);
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
  plat_sock_timeouts(s, timeout_ms, timeout_ms);
  return s;
}

/* ================================================================ misc */
size_t plat_strlcpy(char *dst, const char *src, size_t size) {
  size_t n = strlen(src);
  if (size) {
    size_t c = n < size - 1 ? n : size - 1;
    memcpy(dst, src, c);
    dst[c] = 0;
  }
  return n;
}

#ifdef _WIN32
int plat_spawn(const char *cmdline) {
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  char *c = _strdup(cmdline);
  BOOL ok = CreateProcessA(NULL, c, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
  free(c);
  if (!ok) return -1;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 0;
}
int plat_run_capture(const char *cmdline, char *out, size_t max, int timeout_ms) {
  /* like _popen ("cmd /c <line>"), with a timeout; the job ends what cmd started too */
  SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
  HANDLE rd, wr;
  if (max) out[0] = 0;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  size_t cl = strlen(cmdline) + 16;
  char *c = malloc(cl);
  if (!c) { CloseHandle(rd); CloseHandle(wr); return -1; }
  snprintf(c, cl, "cmd.exe /c %s", cmdline);
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = wr;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  HANDLE job = CreateJobObjectA(NULL, NULL);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof li);
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
  }
  BOOL ok = CreateProcessA(NULL, c, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
  free(c);
  CloseHandle(wr);
  if (!ok) { CloseHandle(rd); if (job) CloseHandle(job); return -1; }
  if (job) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  size_t n = 0;
  bool timed_out = false;
  int64_t end = plat_now_us() + (int64_t)timeout_ms * 1000;
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;     /* every writer closed it */
    if (avail) {
      char tmp[4096];
      DWORD k = 0;
      if (!ReadFile(rd, tmp, avail < sizeof tmp ? avail : (DWORD)sizeof tmp, &k, NULL) || !k) break;
      for (DWORD i = 0; i < k; i++) if (max && n < max - 1) out[n++] = tmp[i];
      continue;
    }
    if (plat_now_us() >= end) { timed_out = true; break; }
    Sleep(10);
  }
  if (max) out[n] = 0;
  CloseHandle(rd);
  if (timed_out) {
    if (job) TerminateJobObject(job, 1);
    else TerminateProcess(pi.hProcess, 1);
  }
  WaitForSingleObject(pi.hProcess, timed_out ? 5000 : INFINITE);
  DWORD code = 0;
  int r = !timed_out && GetExitCodeProcess(pi.hProcess, &code) ? (int)code : -1;
  CloseHandle(pi.hProcess);
  if (job) CloseHandle(job);
  return r;
}
#else
int plat_spawn(const char *cmdline) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    /* grandchild runs the command so nobody has to reap it */
    if (fork() == 0) {
      setsid();
      struct sched_param sp = { 0 };
      sched_setscheduler(0, SCHED_OTHER, &sp);
      for (int fd = 3; fd < 1024; fd++) close(fd);
      execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
      _exit(127);
    }
    _exit(0);
  }
  int st;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  return 0;
}
int plat_run_capture(const char *cmdline, char *out, size_t max, int timeout_ms) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
  if (pid == 0) {
    struct sched_param sp = { 0 };
    sched_setscheduler(0, SCHED_OTHER, &sp);
    dup2(fds[1], 1);
    close(fds[0]); close(fds[1]);
    for (int fd = 3; fd < 1024; fd++) close(fd);
    execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
    _exit(127);
  }
  close(fds[1]);
  size_t n = 0;
  int64_t end = plat_now_us() + (int64_t)timeout_ms * 1000;
  for (;;) {
    int left = (int)((end - plat_now_us()) / 1000);
    if (left <= 0) { kill(pid, SIGKILL); break; }
    struct pollfd p = { fds[0], POLLIN, 0 };
    int r = poll(&p, 1, left);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) { kill(pid, SIGKILL); break; }
    char tmp[4096];
    ssize_t k = read(fds[0], tmp, sizeof tmp);
    if (k <= 0) break;
    for (ssize_t i = 0; i < k; i++) if (max && n < max - 1) out[n++] = tmp[i];
  }
  if (max) out[n] = 0;
  close(fds[0]);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
#endif
