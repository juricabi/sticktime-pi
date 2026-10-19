/* drawing = gpu, the host's side: see gpu.h. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "gpu.h"
#include "gpu_proto.h"
#include "par.h"
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __linux__
struct gpu { int unused; };
gpu_t *gpu_open(const char *helper, char *err, int errlen) {
  (void)helper;
  if (err && errlen > 0) snprintf(err, (size_t)errlen, "GPU drawing needs Linux");
  return NULL;
}
void gpu_close(gpu_t *g) { (void)g; }
int gpu_poll(gpu_t *g) { (void)g; return -1; }
const char *gpu_info(const gpu_t *g) { (void)g; return "GPU drawing needs Linux"; }
int gpu_set_target(gpu_t *g, const int *fds, int nbuf, int w, int h, int pitch, uint32_t fourcc, int rw, int rh,
                   int rows, int pad, int strength, uint32_t id) {
  (void)g; (void)fds; (void)nbuf; (void)w; (void)h; (void)pitch; (void)fourcc; (void)rw; (void)rh; (void)rows;
  (void)pad; (void)strength; (void)id;
  return -1;
}
uint32_t gpu_target_id(const gpu_t *g) { (void)g; return 0; }
void gpu_drop_target(gpu_t *g) { (void)g; }
int gpu_draw(gpu_t *g, const rcmd_t *c, int n, int buf, uint16_t *shot) {
  (void)g; (void)c; (void)n; (void)buf; (void)shot;
  return -1;
}
float gpu_last_ms(const gpu_t *g) { (void)g; return 0; }
#else

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AKEYS 8192                      /* the frame's glyph masks already in the atlas (a power of two) */

struct gpu {
  pid_t pid;
  int sock;
  uint8_t *shm;
  int state;                            /* 0 starting, 1 ready, -1 given up */
  char info[200];
  uint32_t frame;
  float last_ms;
  int rw, rh;
  uint32_t target_id;
  /* the atlas of this frame's glyph masks: shelves, and the masks placed (by their pointer) */
  int ax, ay, arow;
  const uint8_t *akey[AKEYS];
  uint32_t aval[AKEYS], agen[AKEYS], gen;
};

/* the helper's end, off the frame loop: a second for it to go, then SIGKILL (a GPU job may hold a killed
 * process up a while), and reaped */
static void *reap(void *arg) {
  pid_t pid = (pid_t)(intptr_t)arg;
  for (int i = 0; i < 50 && waitpid(pid, NULL, WNOHANG) == 0; i++) usleep(20000);
  if (waitpid(pid, NULL, WNOHANG) == 0) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
  }
  return NULL;
}

static void end_helper(gpu_t *g, bool kill_now) {
  if (g->pid <= 0) return;
  pid_t pid = g->pid;
  g->pid = 0;
  if (kill_now) kill(pid, SIGKILL);
  /* mostly gone within a few ms: waited for here, so a restart (exec) doesn't leave it behind */
  for (int i = 0; i < 25; i++) {
    if (waitpid(pid, NULL, WNOHANG) != 0) return;
    usleep(10000);
  }
  plat_thread t;
  if (plat_thread_start(&t, reap, (void *)(intptr_t)pid, true)) kill(pid, SIGKILL);
}

static void give_up(gpu_t *g, const char *why) {
  if (g->state >= 0 && why) snprintf(g->info, sizeof g->info, "%s", why);
  g->state = -1;
  end_helper(g, true);
}

/* the helper's answer within ms: 0, -1 no answer in time, -2 the helper is gone */
static int wait_msg(gpu_t *g, gpu_msg_t *m, int ms) {
  struct pollfd p = { .fd = g->sock, .events = POLLIN };
  int r;
  do r = poll(&p, 1, ms);
  while (r < 0 && errno == EINTR);
  if (r == 0) return -1;
  if (r < 0) return -2;
  ssize_t n = recv(g->sock, m, sizeof *m, MSG_DONTWAIT);
  return n == (ssize_t)sizeof *m ? 0 : -2;
}

static int send_msg(gpu_t *g, const gpu_msg_t *m, const int *fds, int nfd) {
  char cbuf[CMSG_SPACE(sizeof(int) * GPU_MAX_BUF)];
  struct iovec io = { .iov_base = (void *)m, .iov_len = sizeof *m };
  struct msghdr h = { .msg_iov = &io, .msg_iovlen = 1 };
  if (nfd > 0) {
    memset(cbuf, 0, sizeof cbuf);
    h.msg_control = cbuf;
    h.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfd);
    struct cmsghdr *c = CMSG_FIRSTHDR(&h);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfd);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfd);
  }
  ssize_t r;
  do r = sendmsg(g->sock, &h, MSG_NOSIGNAL);
  while (r < 0 && errno == EINTR);
  return r == (ssize_t)sizeof *m ? 0 : -1;
}

gpu_t *gpu_open(const char *helper, char *err, int errlen) {
  if (access(helper, X_OK)) {
    snprintf(err, (size_t)errlen, "no %s", helper);
    return NULL;
  }
  gpu_t *g = calloc(1, sizeof *g);
  if (!g) {
    snprintf(err, (size_t)errlen, "out of memory");
    return NULL;
  }
  g->sock = -1;
  int sv[2] = { -1, -1 }, mfd = (int)syscall(SYS_memfd_create, "sticktime-gpu", 1u /* MFD_CLOEXEC */);
  if (mfd < 0 || ftruncate(mfd, GPU_SHM_SIZE) || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) ||
      (g->shm = mmap(NULL, GPU_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0)) == MAP_FAILED) {
    snprintf(err, (size_t)errlen, "no shared memory or socket (%s)", strerror(errno));
    if (mfd >= 0) close(mfd);
    if (sv[0] >= 0) { close(sv[0]); close(sv[1]); }
    if (g->shm == MAP_FAILED) g->shm = NULL;
    free(g);
    return NULL;
  }
  /* the helper runs as the pool's threads do (par.c): below the audio thread, on par_plan's CPUs */
  int pol = SCHED_OTHER;
  struct sched_param sp;
  memset(&sp, 0, sizeof sp);
  if (pthread_getschedparam(pthread_self(), &pol, &sp) == 0 && pol == SCHED_FIFO && sp.sched_priority > 2) sp.sched_priority -= 2;
  else { pol = SCHED_OTHER; sp.sched_priority = 0; }
  cpu_set_t cpus, cur;
  int pinned = -1, threads;
  if (sched_getaffinity(0, sizeof cur, &cur) == 0 && CPU_COUNT(&cur) == 1)
    for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &cur)) { pinned = i; break; }
  unsigned long long plan = par_plan(plat_cpus_online(), pinned, &threads);
  CPU_ZERO(&cpus);
  for (int i = 0; i < 64; i++) if (plan >> i & 1) CPU_SET(i, &cpus);
  /* from here to exec only system calls: the host has other threads */
  int a = fcntl(sv[1], F_DUPFD_CLOEXEC, 10), b = a >= 0 ? fcntl(mfd, F_DUPFD_CLOEXEC, 10) : -1;
  pid_t pid = b >= 0 ? fork() : -1;
  if (pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    syscall(SYS_sched_setscheduler, 0, pol, &sp);   /* (musl's sched_setscheduler only fails) */
    sched_setaffinity(0, sizeof cpus, &cpus);
    if (dup2(a, GPU_FD_SOCK) < 0 || dup2(b, GPU_FD_SHM) < 0) _exit(127);
    for (int fd = GPU_FD_SHM + 1; fd < 1024; fd++) close(fd);
    char *argv[] = { (char *)"sticktime-gpu", NULL };
    execv(helper, argv);
    _exit(127);
  }
  if (a >= 0) close(a);
  if (b >= 0) close(b);
  close(sv[1]);
  close(mfd);
  g->sock = sv[0];
  if (pid < 0) {
    snprintf(err, (size_t)errlen, "can't start %s (%s)", helper, strerror(errno));
    gpu_close(g);
    return NULL;
  }
  g->pid = pid;
  gpu_msg_t m;
  memset(&m, 0, sizeof m);
  m.type = GM_HELLO;
  m.proto = GPU_PROTO;
  m.cmd_size = sizeof(rcmd_t);
  m.msg_size = sizeof(gpu_msg_t);
  if (send_msg(g, &m, NULL, 0)) {
    snprintf(err, (size_t)errlen, "%s doesn't answer", helper);
    gpu_close(g);
    return NULL;
  }
  snprintf(g->info, sizeof g->info, "starting");
  return g;
}

void gpu_close(gpu_t *g) {
  if (!g) return;
  if (g->pid > 0 && g->sock >= 0) {
    gpu_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = GM_QUIT;
    send_msg(g, &m, NULL, 0);
  }
  if (g->sock >= 0) close(g->sock);            /* (a helper still setting up sees the socket close) */
  end_helper(g, false);
  if (g->shm) munmap(g->shm, GPU_SHM_SIZE);
  free(g);
}

int gpu_poll(gpu_t *g) {
  if (!g) return -1;
  if (g->state) return g->state;
  gpu_msg_t m;
  ssize_t n = recv(g->sock, &m, sizeof m, MSG_DONTWAIT);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
  if (n != (ssize_t)sizeof m || (m.type != GM_READY && m.type != GM_ERROR)) {
    give_up(g, "the GPU helper stopped");
    return -1;
  }
  m.text[sizeof m.text - 1] = 0;
  if (m.type == GM_ERROR) {
    give_up(g, m.text);
    return -1;
  }
  snprintf(g->info, sizeof g->info, "%s", m.text);
  g->state = 1;
  return 1;
}

const char *gpu_info(const gpu_t *g) { return g ? g->info : "off"; }
float gpu_last_ms(const gpu_t *g) { return g ? g->last_ms : 0; }

int gpu_set_target(gpu_t *g, const int *fds, int nbuf, int w, int h, int pitch, uint32_t fourcc, int rw, int rh,
                   int rows, int pad, int strength, uint32_t id) {
  if (!g || g->state != 1 || nbuf < 1 || nbuf > GPU_MAX_BUF) return -1;
  g->target_id = 0;
  if ((size_t)rw * (size_t)rh * 2 > GPU_SHM_READ_MAX) {     /* its copy for the screenshot wouldn't fit */
    give_up(g, "the render size is too big for the GPU helper");
    return -1;
  }
  gpu_msg_t m;
  memset(&m, 0, sizeof m);
  m.type = GM_TARGET;
  m.proto = GPU_PROTO;
  m.nbuf = nbuf;
  m.w = w;
  m.h = h;
  m.pitch = pitch;
  m.fourcc = fourcc;
  m.rw = rw;
  m.rh = rh;
  m.rows = rows;
  m.pad = pad;
  m.strength = strength;
  int ans = send_msg(g, &m, fds, nbuf) ? -2 : wait_msg(g, &m, 3000);
  if (ans) {
    give_up(g, ans == -1 ? "the GPU helper doesn't answer" : "the GPU helper stopped");
    return -1;
  }
  m.text[sizeof m.text - 1] = 0;
  if (m.type != GM_DONE) {
    give_up(g, m.type == GM_ERROR ? m.text : "the GPU helper said something else");
    return -1;
  }
  g->rw = rw;
  g->rh = rh;
  g->target_id = id;
  return 0;
}

uint32_t gpu_target_id(const gpu_t *g) { return g ? g->target_id : 0; }

void gpu_drop_target(gpu_t *g) {
  if (!g || g->state != 1 || !g->target_id) return;
  g->target_id = 0;
  gpu_msg_t m;
  memset(&m, 0, sizeof m);
  m.type = GM_TARGET;
  m.proto = GPU_PROTO;
  int w = send_msg(g, &m, NULL, 0) ? -2 : wait_msg(g, &m, 1000);
  if (w || m.type != GM_DONE) give_up(g, w == -1 ? "the GPU helper doesn't answer" : "the GPU helper stopped");
}

/* a glyph mask's place in this frame's atlas (x | y << 16), placed now if new; -1: no room */
static int64_t atlas_put(gpu_t *g, const glyph_t *gl) {
  uint32_t k = (uint32_t)(((uint64_t)(uintptr_t)gl->a * 0x9E3779B97F4A7C15ull) >> 51) & (AKEYS - 1);
  for (int probe = 0; probe < AKEYS; probe++, k = (k + 1) & (AKEYS - 1)) {
    if (g->agen[k] != g->gen) break;
    if (g->akey[k] == gl->a) return g->aval[k];
  }
  if (g->agen[k] == g->gen) return -1;          /* full */
  int w = gl->w, h = gl->h;
  if (w > GPU_ATLAS_W) return -1;
  if (g->ax + w > GPU_ATLAS_W) {
    g->ay += g->arow;
    g->ax = 0;
    g->arow = 0;
  }
  if (g->ay + h > GPU_ATLAS_MAXH) return -1;
  for (int j = 0; j < h; j++)
    memcpy(g->shm + GPU_SHM_ATLAS + (size_t)(g->ay + j) * GPU_ATLAS_W + (size_t)g->ax, gl->a + (size_t)j * (size_t)w, (size_t)w);
  uint32_t place = (uint32_t)g->ax | (uint32_t)g->ay << 16;
  g->ax += w;
  if (h > g->arow) g->arow = h;
  g->akey[k] = gl->a;
  g->aval[k] = place;
  g->agen[k] = g->gen;
  return place;
}

int gpu_draw(gpu_t *g, const rcmd_t *c, int n, int buf, uint16_t *shot) {
  if (!g || g->state != 1) return -1;
  int max = (int)(GPU_SHM_CMD_MAX / sizeof(rcmd_t));
  if (n > max) n = max;                         /* (a frame of 100 000 calls: the rest is lost) */
  rcmd_t *d = (rcmd_t *)(g->shm + GPU_SHM_CMD);
  g->gen++;
  if (!g->gen) {                                /* wrapped: every slot looks new again */
    memset(g->agen, 0, sizeof g->agen);
    g->gen = 1;
  }
  g->ax = g->ay = g->arow = 0;
  for (int i = 0; i < n; i++) {
    d[i] = c[i];
    if (c[i].op != RC_GLYPH) continue;
    int64_t at = c[i].u.gl.g.a && c[i].u.gl.g.w > 0 && c[i].u.gl.g.h > 0 ? atlas_put(g, &c[i].u.gl.g) : -1;
    if (at < 0) d[i].u.gl.g.w = 0;              /* not drawn */
    else d[i].u.gl.g.a = (uint8_t *)(uintptr_t)at;
  }
  gpu_msg_t m;
  memset(&m, 0, sizeof m);
  m.type = GM_FRAME;
  m.proto = GPU_PROTO;
  m.frame = ++g->frame;
  m.buf = buf;
  m.ncmd = (uint32_t)n;
  m.atlas_h = g->ay + g->arow;
  m.readback = shot != NULL;
  if (send_msg(g, &m, NULL, 0)) {
    give_up(g, "the GPU helper stopped");
    return -1;
  }
  /* a frame takes a few ms; half a second is a GPU that hangs */
  for (;;) {
    int w = wait_msg(g, &m, 500);
    if (w) {
      give_up(g, w == -1 ? "the GPU took too long" : "the GPU helper stopped");
      return -1;
    }
    m.text[sizeof m.text - 1] = 0;
    if (m.type == GM_ERROR) {
      give_up(g, m.text);
      return -1;
    }
    if (m.type == GM_DONE && m.frame == g->frame) break;
  }
  g->last_ms = m.gpu_ms;
  if (shot) memcpy(shot, g->shm + GPU_SHM_READ, (size_t)g->rw * (size_t)g->rh * 2);
  return 0;
}
#endif
