/* KMS video output with raw ioctls (no libdrm). See video.h; the spec with the kernel source
 * references is the research note drm-kms.md (sections in brackets below).
 *
 * Legacy KMS only (no atomic): find the KMS card, pick the connector (composite reports status
 * "unknown"), its CRTC and mode, set the TV norm via the connector's "TV mode" property, two
 * dumb buffers at the render size (RGB565: the game's own format, so the copy is a memcpy),
 * SETCRTC with a black mode-size buffer and SETPLANE on the primary plane so the HVS scales the
 * render buffer into the view; page flips keep the plane's rectangles. If SETPLANE can't scale
 * (vkms, USB displays) the buffers are mode-size and the CPU scales with LUTs.
 * Recovery is always a full re-init (new buffers, SETCRTC, SETPLANE), with back-off; while the
 * display is down a software clock keeps the frame loop paced. */
#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "video.h"
#include "plat.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __linux__

video_t *video_drm_open(const video_config_t *c, video_info_t *info, char *err, int errlen) {
  (void)c; (void)info;
  if (err && errlen > 0) snprintf(err, (size_t)errlen, "DRM/KMS needs Linux");
  return NULL;
}

#else

#include <ctype.h>
#include <dirent.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <linux/netlink.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#define U64P(p) ((uint64_t)(uintptr_t)(p))
enum { CONN_CONNECTED = 1, CONN_DISCONNECTED = 2, CONN_UNKNOWN = 3 };   /* enum drm_connector_status */
enum { CLS_OTHER, CLS_COMPOSITE, CLS_HDMI };
enum { OUT_AUTO, OUT_AV, OUT_HDMI };
#define PLANE_PRIMARY 1
#define NBUF 2
#define MAXMODES 256
#define MAXOBJ 32
#define MAXENC 8
#define MAXPLANES 128

/* drm_setup() flags */
#define SETUP_PROBE    1   /* forced connector probe (EDID); init and after a hotplug only */
#define SETUP_DISABLE  2   /* switch the CRTC off first, so the re-enable is a full modeset */

typedef struct { uint32_t handle, fb_id, pitch, w, h; uint64_t size; uint8_t *map; } dbuf_t;

typedef struct {
  uint32_t id, type, type_id, status, enc_id, nenc, nmodes;
  uint32_t enc[MAXENC];
  video_mode_t modes[MAXMODES];          /* filled by GETCONNECTOR (same layout) */
} conn_t;

typedef struct { uint32_t crtc[MAXOBJ], conn[MAXOBJ], ncrtc, nconn; } res_t;

typedef struct { video_mode_t m[VIDEO_MAX_CANDIDATES]; int n; } cands_t;

typedef struct {
  video_t base;
  /* settings (copied: the caller's strings may go away) */
  char device[64], tv[16], aspect[8], hdmi_mode[32];
  int out, margin, cfg_rw, cfg_rh, wait_ms;
  bool progressive, sharp, filter_set;
  bool detail_high;                /* av_detail = high: a progressive composite mode draws twice its lines */
  bool doubled;                    /* the render size is such a doubled one */
  /* device */
  int fd;
  char path[32], driver[32];
  bool master, ts_mono, warned_type, warned_hdmi_mode, hw_scale_broken, have_saved, modeset_done;
  int master_err;                  /* last SET_MASTER error logged (no repeats) */
  int bpp;                         /* 16 (RGB565) unless the primary plane can't: 32 (XRGB8888) */
  /* current output */
  uint32_t conn_id, conn_type, conn_type_id, crtc_id, crtc_idx, plane_id;
  bool composite, blind, active;
  video_mode_t mode;
  int rw, rh;                      /* render size: fixed once chosen */
  video_layout_t lay;
  bool hw_scale, cpu_scale;        /* cpu_scale: mode-size buffers + LUTs (or offset 1:1 copy) */
  dbuf_t buf[NBUF], black, steal;
  uint16_t *xlut, *ylut;
  int front, pending;
  uint32_t gen;
  res_t res;
  conn_t scan, best;               /* scratch for connector selection */
  /* timing */
  int64_t mode_period_us;          /* one refresh by the mode's timings */
  bool have_seq, flip_on_time;     /* have_seq: a flip since the mode set (its time is last_vblank_us) */
  int64_t flip_issued_us, flip_ok_streak;
  uint32_t good_flips;
  int force_flip_err;              /* test hooks */
  bool lose_event;
  /* recovery */
  int backoff_ms, fails_since_ok;
  int64_t retry_at_us, next_check_us, next_nl_us, holder_logged_us;
  bool bg;                         /* our VT is not in front: hands off */
  char down_why[96];
  /* hotplug */
  int nl_fd;
  bool hotplug;
  uint32_t hp_ids[MAXOBJ], hp_status[MAXOBJ], hp_n;
  plat_thread probe_th;
  int probe_state;                 /* 0 idle, 1 running, 2 done (atomic) */
  int probe_fd;
  uint32_t probe_ids[MAXOBJ], probe_n;
  /* the console's CRTC state at start, for the exit without a VT (spec 12) */
  struct drm_mode_crtc saved;
  uint32_t saved_conn;
} drm_t;

static const char *const conn_names[] = {
  "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS", "Component",
  "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB",
};

static const char *conn_type_name(uint32_t t) {
  return t < sizeof conn_names / sizeof conn_names[0] ? conn_names[t] : "Unknown";
}

static int conn_class(uint32_t t) {
  switch (t) {
  case DRM_MODE_CONNECTOR_Composite: case DRM_MODE_CONNECTOR_SVIDEO:
  case DRM_MODE_CONNECTOR_Component: case DRM_MODE_CONNECTOR_TV:
    return CLS_COMPOSITE;
  case DRM_MODE_CONNECTOR_HDMIA: case DRM_MODE_CONNECTOR_HDMIB:
    return CLS_HDMI;
  default:
    return CLS_OTHER;
  }
}

static int xioctl(int fd, unsigned long req, void *arg) {
  int r;
  do r = ioctl(fd, req, arg);
  while (r == -1 && (errno == EINTR || errno == EAGAIN));
  return r == -1 ? -errno : 0;
}

/* ================================================================ modes */

/* the portable selection in video.c works on video_mode_t: the same bytes as the kernel's */
_Static_assert(sizeof(video_mode_t) == sizeof(struct drm_mode_modeinfo), "video_mode_t layout");
_Static_assert(offsetof(video_mode_t, hskew) == offsetof(struct drm_mode_modeinfo, hskew), "video_mode_t layout");
_Static_assert(offsetof(video_mode_t, vscan) == offsetof(struct drm_mode_modeinfo, vscan), "video_mode_t layout");
_Static_assert(offsetof(video_mode_t, vrefresh) == offsetof(struct drm_mode_modeinfo, vrefresh), "video_mode_t layout");
_Static_assert(offsetof(video_mode_t, type) == offsetof(struct drm_mode_modeinfo, type), "video_mode_t layout");
_Static_assert(offsetof(video_mode_t, name) == offsetof(struct drm_mode_modeinfo, name), "video_mode_t layout");
_Static_assert(VIDEO_MODE_FLAG_INTERLACE == DRM_MODE_FLAG_INTERLACE && VIDEO_MODE_FLAG_DBLSCAN == DRM_MODE_FLAG_DBLSCAN &&
               VIDEO_MODE_FLAG_PIC_AR_MASK == DRM_MODE_FLAG_PIC_AR_MASK && VIDEO_MODE_FLAG_NHSYNC == DRM_MODE_FLAG_NHSYNC &&
               VIDEO_MODE_FLAG_PVSYNC == DRM_MODE_FLAG_PVSYNC && VIDEO_MODE_TYPE_PREFERRED == DRM_MODE_TYPE_PREFERRED &&
               VIDEO_MODE_TYPE_USERDEF == DRM_MODE_TYPE_USERDEF, "mode flag values");

static bool mode_il(const video_mode_t *m) { return (m->flags & VIDEO_MODE_FLAG_INTERLACE) != 0; }
static bool mode_pref(const video_mode_t *m) { return (m->type & VIDEO_MODE_TYPE_PREFERRED) != 0; }

static void composite_cands(drm_t *d, const conn_t *c, cands_t *o) {
  o->n = video_composite_modes(c->modes, (int)c->nmodes, d->tv, d->progressive, o->m);
}

static void hdmi_cands(drm_t *d, const conn_t *c, cands_t *o) {
  int note = 0;
  o->n = video_hdmi_modes(c->modes, (int)c->nmodes, d->hdmi_mode, o->m, &note);
  if (note && !d->warned_hdmi_mode) {
    if (note == 1) video_log(&d->base, "hdmi_mode '%s' not understood, using auto", d->hdmi_mode);
    else video_log(&d->base, "hdmi_mode %s: the display doesn't offer it", d->hdmi_mode);
    d->warned_hdmi_mode = true;
  }
}

/* ================================================================ device */

static double uptime_s(void) {
  FILE *f = fopen("/proc/uptime", "r");
  double u = 1e9;
  if (f) {
    if (fscanf(f, "%lf", &u) != 1) u = 1e9;
    fclose(f);
  }
  return u;
}

static int get_resources(int fd, res_t *r) {
  for (int tries = 0; tries < 5; tries++) {
    struct drm_mode_card_res res = { 0 };
    int e = xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    if (e) return e;
    uint32_t nc = res.count_crtcs, nn = res.count_connectors;
    if (nc > MAXOBJ) nc = MAXOBJ;     /* partial copies are allowed here */
    if (nn > MAXOBJ) nn = MAXOBJ;
    struct drm_mode_card_res r2 = { .crtc_id_ptr = U64P(r->crtc), .count_crtcs = nc,
                                    .connector_id_ptr = U64P(r->conn), .count_connectors = nn };
    if ((e = xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2))) return e;
    if ((r2.count_crtcs > nc && nc < MAXOBJ) || (r2.count_connectors > nn && nn < MAXOBJ)) continue;  /* grew: retry */
    r->ncrtc = r2.count_crtcs < nc ? r2.count_crtcs : nc;
    r->nconn = r2.count_connectors < nn ? r2.count_connectors : nn;
    return 0;
  }
  return -EAGAIN;
}

/* force: count_modes = 0 makes the kernel probe (detect + EDID, slow, master only) [2] */
static int get_connector(int fd, uint32_t id, bool force, conn_t *c) {
  video_mode_t dummy;
  struct drm_mode_get_connector g = { .connector_id = id };
  if (!force) {
    g.count_modes = 1;
    g.modes_ptr = U64P(&dummy);
  }
  int e = xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &g);
  if (e) return e;
  for (int tries = 0; tries < 5; tries++) {
    uint32_t nm = g.count_modes, ne = g.count_encoders;
    if (nm > MAXMODES) nm = MAXMODES;   /* the copy is all-or-nothing: then we get no modes */
    if (ne > MAXENC) ne = MAXENC;
    struct drm_mode_get_connector h = {
      .connector_id = id,
      .count_modes = nm ? nm : 1,            /* never 0: that would probe again */
      .modes_ptr = U64P(nm ? c->modes : &dummy),
      .count_encoders = ne,
      .encoders_ptr = U64P(c->enc),
    };
    if ((e = xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &h))) return e;
    if ((h.count_modes > nm && nm < MAXMODES) || (h.count_encoders > ne && ne < MAXENC)) {
      g = h;                                  /* counts grew between the calls: again */
      continue;
    }
    c->id = id;
    c->type = h.connector_type;
    c->type_id = h.connector_type_id;
    c->status = h.connection;
    c->enc_id = h.encoder_id;
    c->nenc = h.count_encoders <= ne ? h.count_encoders : 0;
    c->nmodes = nm && h.count_modes <= nm ? h.count_modes : 0;
    return 0;
  }
  return -EAGAIN;
}

static void conn_copy(conn_t *dst, const conn_t *src) {
  memcpy(dst, src, offsetof(conn_t, modes));
  memcpy(dst->modes, src->modes, sizeof src->modes[0] * src->nmodes);
}

static void conn_name(const conn_t *c, char *out, size_t n) {
  snprintf(out, n, "%s-%u", conn_type_name(c->type), c->type_id);
}

static int find_prop(int fd, uint32_t obj, uint32_t type, const char *name, uint32_t *prop_id, uint64_t *value) {
  uint32_t ids[64];
  uint64_t vals[64];
  struct drm_mode_obj_get_properties op = { .props_ptr = U64P(ids), .prop_values_ptr = U64P(vals),
                                            .count_props = 64, .obj_id = obj, .obj_type = type };
  int e = xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op);
  if (e) return e;
  uint32_t n = op.count_props < 64 ? op.count_props : 64;
  for (uint32_t i = 0; i < n; i++) {
    struct drm_mode_get_property gp = { .prop_id = ids[i] };    /* counts only, no arrays */
    if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp)) continue;
    if (!strncmp(gp.name, name, DRM_PROP_NAME_LEN)) {
      *prop_id = ids[i];
      if (value) *value = vals[i];
      return 0;
    }
  }
  return -ENOENT;
}

static int prop_enum_value(int fd, uint32_t prop_id, const char *ename, uint64_t *out) {
  struct drm_mode_property_enum en[32];
  uint64_t vals[32];
  struct drm_mode_get_property gp = { .prop_id = prop_id, .values_ptr = U64P(vals), .count_values = 32,
                                      .enum_blob_ptr = U64P(en), .count_enum_blobs = 32 };
  int e = xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp);
  if (e) return e;
  if (!(gp.flags & DRM_MODE_PROP_ENUM)) return -EINVAL;
  uint32_t n = gp.count_enum_blobs < 32 ? gp.count_enum_blobs : 32;
  for (uint32_t i = 0; i < n; i++)
    if (!strncmp(en[i].name, ename, DRM_PROP_NAME_LEN)) {
      *out = en[i].value;
      return 0;
    }
  return -ENOENT;
}

/* set an enum property by value name; 0 when it is (now) set, -ENOENT when there's no such
 * property or value. Blocking: may be a full modeset (TV mode). */
static int set_enum_prop(int fd, uint32_t obj, uint32_t type, const char *prop, const char *ename) {
  uint32_t pid;
  uint64_t cur, want;
  int e = find_prop(fd, obj, type, prop, &pid, &cur);
  if (e || (e = prop_enum_value(fd, pid, ename, &want))) return e;
  if (cur == want) return 0;
  struct drm_mode_obj_set_property sp = { .value = want, .prop_id = pid, .obj_id = obj, .obj_type = type };
  return xioctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &sp);
}

static bool plane_has_format(int fd, uint32_t plane, uint32_t fmt, bool *known) {
  uint32_t fmts[128];
  struct drm_mode_get_plane gp = { .plane_id = plane };
  *known = false;
  if (xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp) || !gp.count_format_types || gp.count_format_types > 128) return false;
  struct drm_mode_get_plane g2 = { .plane_id = plane, .count_format_types = gp.count_format_types,
                                   .format_type_ptr = U64P(fmts) };
  if (xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &g2) || g2.count_format_types > gp.count_format_types) return false;
  *known = true;
  for (uint32_t i = 0; i < g2.count_format_types; i++)
    if (fmts[i] == fmt) return true;
  return false;
}

static uint32_t find_primary_plane(int fd, uint32_t crtc_idx) {
  uint32_t ids[MAXPLANES];
  struct drm_mode_get_plane_res pr = { .plane_id_ptr = U64P(ids), .count_planes = MAXPLANES };
  if (xioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) return 0;
  uint32_t n = pr.count_planes < MAXPLANES ? pr.count_planes : MAXPLANES;
  for (uint32_t i = 0; i < n; i++) {
    struct drm_mode_get_plane gp = { .plane_id = ids[i] };
    uint32_t pid;
    uint64_t type;
    if (xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp) || crtc_idx >= 32 || !(gp.possible_crtcs & (1u << crtc_idx))) continue;
    if (!find_prop(fd, ids[i], DRM_MODE_OBJECT_PLANE, "type", &pid, &type) && type == PLANE_PRIMARY) return ids[i];
  }
  return 0;
}

/* without the "type" property: the plane that shows fb on our CRTC right after SETCRTC [4.11] */
static uint32_t plane_showing(int fd, uint32_t crtc_id, uint32_t fb) {
  uint32_t ids[MAXPLANES];
  struct drm_mode_get_plane_res pr = { .plane_id_ptr = U64P(ids), .count_planes = MAXPLANES };
  if (xioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) return 0;
  uint32_t n = pr.count_planes < MAXPLANES ? pr.count_planes : MAXPLANES;
  for (uint32_t i = 0; i < n; i++) {
    struct drm_mode_get_plane gp = { .plane_id = ids[i] };
    if (!xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp) && gp.crtc_id == crtc_id && gp.fb_id == fb) return ids[i];
  }
  return 0;
}

/* a KMS card can make dumb buffers and has CRTCs and connectors (v3d is render-only) */
static int kms_open(const char *path, char *driver, size_t dlen, int *err_out) {
  int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    *err_out = -errno;
    return -1;
  }
  struct drm_get_cap cap = { .capability = DRM_CAP_DUMB_BUFFER };
  struct drm_mode_card_res res = { 0 };
  if (xioctl(fd, DRM_IOCTL_GET_CAP, &cap) || !cap.value || xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) ||
      !res.count_connectors || !res.count_crtcs) {
    close(fd);
    *err_out = -ENOTSUP;
    return -1;
  }
  char name[32] = { 0 };
  struct drm_version ver = { .name_len = sizeof name - 1, .name = name };
  if (xioctl(fd, DRM_IOCTL_VERSION, &ver)) name[0] = 0;
  plat_strlcpy(driver, name, dlen);
  return fd;
}

/* does the card have a connector of the configured kind (cached reads, no master needed) */
static bool card_has_class(drm_t *d, int fd, int cls) {
  res_t r;
  if (get_resources(fd, &r)) return false;
  for (uint32_t i = 0; i < r.nconn; i++)
    if (!get_connector(fd, r.conn[i], false, &d->scan) && conn_class(d->scan.type) == cls) return true;
  return false;
}

static void log_master_holder(drm_t *d) {
  int64_t now = plat_now_us();
  if (d->holder_logged_us && now - d->holder_logged_us < 30000000) return;
  d->holder_logged_us = now;
  DIR *p = opendir("/proc");
  if (!p) return;
  struct dirent *e;
  int hits = 0;
  while ((e = readdir(p)) && hits < 3) {
    if (!isdigit((unsigned char)e->d_name[0]) || atoi(e->d_name) == getpid()) continue;
    char dir[64], lnk[96], tgt[64];
    snprintf(dir, sizeof dir, "/proc/%s/fd", e->d_name);
    DIR *f = opendir(dir);
    if (!f) continue;
    struct dirent *g;
    bool found = false;
    while (!found && (g = readdir(f))) {
      snprintf(lnk, sizeof lnk, "%s/%s", dir, g->d_name);
      ssize_t n = readlink(lnk, tgt, sizeof tgt - 1);
      if (n > 0) {
        tgt[n] = 0;
        found = !strcmp(tgt, d->path);
      }
    }
    closedir(f);
    if (!found) continue;
    char comm[64] = "?", cp[64];
    snprintf(cp, sizeof cp, "/proc/%s/comm", e->d_name);
    FILE *c = fopen(cp, "r");
    if (c) {
      if (fgets(comm, sizeof comm, c)) comm[strcspn(comm, "\n")] = 0;
      fclose(c);
    }
    video_log(&d->base, "%s is held by pid %s (%s)", d->path, e->d_name, comm);
    hits++;
  }
  closedir(p);
}

static int try_master(drm_t *d, int total_ms) {
  int e = 0;
  for (int t = 0;; t += 100) {
    e = xioctl(d->fd, DRM_IOCTL_SET_MASTER, NULL);    /* 0 when already master (open made us) */
    if (!e || e != -EBUSY || t >= total_ms) break;
    plat_sleep_us(100000);
  }
  d->master = e == 0;
  if (e && e != d->master_err) {
    if (e == -EBUSY) {
      video_log(&d->base, "another program is DRM master on %s", d->path);
      log_master_holder(d);
    } else {
      video_log(&d->base, "SET_MASTER on %s: %s", d->path, strerror(-e));
    }
  }
  d->master_err = e;
  return e;
}

static void probe_join(drm_t *d) {
  if (__atomic_load_n(&d->probe_state, __ATOMIC_SEQ_CST)) {
    plat_thread_join(d->probe_th);
    __atomic_store_n(&d->probe_state, 0, __ATOMIC_SEQ_CST);
  }
}

static void buf_destroy(int fd, dbuf_t *b) {
  if (b->map) munmap(b->map, (size_t)b->size);
  if (fd >= 0 && b->fb_id) {
    uint32_t id = b->fb_id;
    xioctl(fd, DRM_IOCTL_MODE_RMFB, &id);
  }
  if (fd >= 0 && b->handle) {
    struct drm_mode_destroy_dumb dd = { .handle = b->handle };
    xioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
  }
  memset(b, 0, sizeof *b);
}

static void destroy_all(drm_t *d, dbuf_t *bufs, dbuf_t *black) {
  for (int i = 0; i < NBUF; i++) buf_destroy(d->fd, &bufs[i]);
  buf_destroy(d->fd, black);
}

static void close_card(drm_t *d) {
  probe_join(d);
  destroy_all(d, d->buf, &d->black);
  buf_destroy(d->fd, &d->steal);
  if (d->fd >= 0) close(d->fd);
  d->fd = -1;
  d->master = d->active = false;
  d->hw_scale_broken = false;
  d->plane_id = 0;
  d->pending = -1;
  d->front = -1;
  d->gen++;
}

/* find and open the KMS card, waiting while the system boots [4.1] */
static int open_card(drm_t *d, int wait_ms) {
  int64_t end = plat_now_us() + (int64_t)wait_ms * 1000;
  int want = d->out == OUT_AV ? CLS_COMPOSITE : d->out == OUT_HDMI ? CLS_HDMI : -1;
  bool logged = false, logged_acc = false;
  int last_err = -ENOENT;
  for (;;) {
    int best_fd = -1, best_score = -1;
    char best_path[32] = "", best_drv[32] = "";
    for (int i = 0; i < 16; i++) {
      char path[32], drv[32];
      if (d->device[0]) {
        if (i) break;
        plat_strlcpy(path, d->device, sizeof path);
      } else {
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
      }
      int e = 0, fd = kms_open(path, drv, sizeof drv, &e);
      if (fd < 0) {
        if ((e == -EACCES || e == -EPERM) && !logged_acc) {
          video_log(&d->base, "%s: %s", path, strerror(-e));
          logged_acc = true;
        }
        if (e != -ENOENT || last_err == -ENOENT) last_err = e;
        continue;
      }
      /* prefer the configured connector type, then vc4 (USB displays may come first) */
      int score = (want >= 0 && card_has_class(d, fd, want) ? 2 : 0) + (!strcmp(drv, "vc4") ? 1 : 0);
      if (score > best_score) {
        if (best_fd >= 0) close(best_fd);   /* never keep two fds of one card: only one is master */
        best_fd = fd;
        best_score = score;
        plat_strlcpy(best_path, path, sizeof best_path);
        plat_strlcpy(best_drv, drv, sizeof best_drv);
      } else {
        close(fd);
      }
    }
    if (best_fd >= 0) {
      d->fd = best_fd;
      plat_strlcpy(d->path, best_path, sizeof d->path);
      plat_strlcpy(d->driver, best_drv, sizeof d->driver);
      break;
    }
    if (plat_now_us() >= end) return last_err;
    if (!logged) {
      video_log(&d->base, "waiting up to %d s for a KMS device", wait_ms / 1000);
      logged = true;
    }
    plat_sleep_us(100000);
  }
  struct drm_get_cap cap = { .capability = DRM_CAP_TIMESTAMP_MONOTONIC };
  d->ts_mono = !xioctl(d->fd, DRM_IOCTL_GET_CAP, &cap) && cap.value;
  /* universal planes: GETPLANERESOURCES lists the primary plane; no ATOMIC, no ASPECT_RATIO */
  struct drm_set_client_cap cc = { .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1 };
  xioctl(d->fd, DRM_IOCTL_SET_CLIENT_CAP, &cc);
  d->gen++;
  return 0;
}

/* ================================================================ selection + modeset */

static int conn_score(const drm_t *d, const conn_t *c) {
  int cls = conn_class(c->type);
  bool on = c->status == CONN_CONNECTED;
  bool usable = on || (c->status == CONN_UNKNOWN && (cls == CLS_COMPOSITE || c->nmodes));
  if (c->type == DRM_MODE_CONNECTOR_WRITEBACK) return -1;
  int s;
  if (d->out == OUT_AV) s = cls == CLS_COMPOSITE ? (usable ? 100 : 50) : (on ? 20 : usable ? 10 : 1);
  else if (d->out == OUT_HDMI) s = cls == CLS_HDMI ? (on ? 100 : 40) : (on ? 20 : usable ? 10 : 1);
  else s = on ? 90 : (cls == CLS_COMPOSITE && usable) ? 80 : usable ? 60 : cls == CLS_HDMI ? 30 : 1;
  return s * 64 - (int)(c->type_id > 63 ? 63 : c->type_id);   /* HDMI-A-1 before HDMI-A-2 */
}

/* pick the connector into d->best; returns its score or -ENODEV */
static int pick_connector(drm_t *d, bool force) {
  int best = -1;
  for (uint32_t i = 0; i < d->res.nconn; i++) {
    if (get_connector(d->fd, d->res.conn[i], force, &d->scan)) continue;
    int s = conn_score(d, &d->scan);
    if (s > best) {
      best = s;
      conn_copy(&d->best, &d->scan);
    }
  }
  return best < 0 ? -ENODEV : best;
}

static int pick_crtc(drm_t *d, const conn_t *c, uint32_t *crtc_id, uint32_t *crtc_idx) {
  for (int pass = 0; pass < 2; pass++)
    for (uint32_t i = 0; i < c->nenc; i++) {
      struct drm_mode_get_encoder e = { .encoder_id = c->enc[i] };
      if (xioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &e)) continue;
      for (uint32_t k = 0; k < d->res.ncrtc && k < 32; k++) {
        /* pass 0: the CRTC already driving it (fbcon), pass 1: the first possible one */
        bool ok = pass == 0 ? (e.encoder_id == c->enc_id && e.crtc_id && e.crtc_id == d->res.crtc[k])
                            : (e.possible_crtcs & (1u << k)) != 0;
        if (ok) {
          *crtc_id = d->res.crtc[k];
          *crtc_idx = k;
          return 0;
        }
      }
    }
  return -ENOENT;
}

static int buf_create(drm_t *d, dbuf_t *b, uint32_t w, uint32_t h) {
  /* allocation width rounded to 16 px: a 32-byte aligned pitch for the HVS and the copy */
  struct drm_mode_create_dumb cd = { .width = (w + 15) & ~15u, .height = h, .bpp = (uint32_t)d->bpp };
  int e = xioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd);
  if (e) return e;
  *b = (dbuf_t){ .handle = cd.handle, .pitch = cd.pitch, .size = cd.size, .w = w, .h = h };
  struct drm_mode_fb_cmd2 f = { .width = w, .height = h,
                                .pixel_format = d->bpp == 16 ? DRM_FORMAT_RGB565 : DRM_FORMAT_XRGB8888,
                                .handles = { cd.handle }, .pitches = { cd.pitch } };
  if ((e = xioctl(d->fd, DRM_IOCTL_MODE_ADDFB2, &f))) {
    struct drm_mode_fb_cmd o = { .width = w, .height = h, .pitch = cd.pitch, .bpp = (uint32_t)d->bpp,
                                 .depth = d->bpp == 16 ? 16u : 24u, .handle = cd.handle };
    if ((e = xioctl(d->fd, DRM_IOCTL_MODE_ADDFB, &o))) goto fail;
    f.fb_id = o.fb_id;
  }
  b->fb_id = f.fb_id;
  struct drm_mode_map_dumb md = { .handle = cd.handle };
  if ((e = xioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &md))) goto fail;
  void *p = mmap(NULL, (size_t)cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t)md.offset);
  if (p == MAP_FAILED) {
    e = -errno;
    goto fail;
  }
  b->map = p;
  memset(b->map, 0, (size_t)cd.size);   /* write-combined memory: written, never read */
  return 0;
fail:
  buf_destroy(d->fd, b);
  return e;
}

static int create_set(drm_t *d, dbuf_t *bufs, dbuf_t *black, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh) {
  int e = 0;
  for (int i = 0; i < NBUF && !e; i++) e = buf_create(d, &bufs[i], w, h);
  if (!e && bw) e = buf_create(d, black, bw, bh);
  if (e) destroy_all(d, bufs, black);
  return e;
}

static void set_scaling_filter(drm_t *d) {
  if (!d->filter_set || !d->plane_id) return;
  int e = set_enum_prop(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "SCALING_FILTER", d->sharp ? "Nearest Neighbor" : "Default");
  if (e && e != -ENOENT) video_log(&d->base, "SCALING_FILTER: %s", strerror(-e));
}

/* Put mode m on screen with fresh buffers. The old buffers are freed after the new ones are on
 * screen (no black flash), or first when memory is short. */
static int apply_mode(drm_t *d, const video_mode_t *m, const conn_t *c, uint32_t crtc_id, uint32_t crtc_idx) {
  int mw = m->hdisplay, mh = m->vdisplay;
  bool composite = conn_class(c->type) == CLS_COMPOSITE;
  video_layout_t lay;
  video_layout(mw, mh, composite, d->aspect, d->margin, d->rw, d->rh, &lay);
  bool full = lay.view_x == 0 && lay.view_y == 0 && lay.view_w == mw && lay.view_h == mh;
  bool exact = full && d->rw == mw && d->rh == mh;
  uint32_t plane = d->plane_id && crtc_idx == d->crtc_idx ? d->plane_id : find_primary_plane(d->fd, crtc_idx);
  if (plane && d->bpp == 16) {
    bool known, has = plane_has_format(d->fd, plane, DRM_FORMAT_RGB565, &known);
    if (known && !has) {
      video_log(&d->base, "the primary plane has no RGB565: using XRGB8888");
      d->bpp = 32;
    }
  }
  uint32_t conn = c->id;
  for (int attempt = 0; attempt < 3; attempt++) {
    /* attempt 0: HVS scaling (or exact 1:1); then CPU scaling into mode-size buffers */
    bool hw = !exact && !d->hw_scale_broken && attempt == 0;
    dbuf_t nb[NBUF] = { { 0 } }, nblack = { 0 };
    uint32_t bw = hw || exact ? (uint32_t)d->rw : (uint32_t)mw, bh = hw || exact ? (uint32_t)d->rh : (uint32_t)mh;
    bool need_black = hw && ((int)bw < mw || (int)bh < mh);   /* SETCRTC's fb must cover the mode [4.10] */
    int e = create_set(d, nb, &nblack, bw, bh, need_black ? (uint32_t)mw : 0, (uint32_t)mh);
    if (e == -ENOMEM || e == -ENOSPC) {        /* CMA short: free the old set first */
      destroy_all(d, d->buf, &d->black);
      d->active = false;
      e = create_set(d, nb, &nblack, bw, bh, need_black ? (uint32_t)mw : 0, (uint32_t)mh);
    }
    if (e == -EINVAL && d->bpp == 16) {         /* no RGB565 anywhere: XRGB8888 */
      d->bpp = 32;
      attempt--;
      continue;
    }
    if (e) return e;
    uint32_t base_fb = need_black ? nblack.fb_id : nb[0].fb_id;
    struct drm_mode_crtc sc = { .set_connectors_ptr = U64P(&conn), .count_connectors = 1, .crtc_id = crtc_id,
                                .fb_id = base_fb, .mode_valid = 1 };
    memcpy(&sc.mode, m, sizeof sc.mode);
    e = xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &sc);
    if (e == -EINVAL && d->bpp == 16 && attempt == 0 && !plane) {
      /* maybe the plane can't show RGB565 (no format list to check): once with XRGB8888 */
      destroy_all(d, nb, &nblack);
      d->bpp = 32;
      attempt--;
      continue;
    }
    if (e) {
      destroy_all(d, nb, &nblack);
      return e;                                  /* EINVAL: the caller tries the next mode */
    }
    d->modeset_done = true;                      /* from now on the CRTC may show our buffers */
    if (!plane) plane = plane_showing(d->fd, crtc_id, base_fb);
    if (hw) {
      struct drm_mode_set_plane sp = {
        .plane_id = plane, .crtc_id = crtc_id, .fb_id = nb[0].fb_id, .flags = 0,
        .crtc_x = lay.view_x, .crtc_y = lay.view_y, .crtc_w = (uint32_t)lay.view_w, .crtc_h = (uint32_t)lay.view_h,
        .src_x = 0, .src_y = 0, .src_w = (uint32_t)d->rw << 16, .src_h = (uint32_t)d->rh << 16,
      };
      e = plane ? xioctl(d->fd, DRM_IOCTL_MODE_SETPLANE, &sp) : -ENOENT;
      if (e) {
        /* the plane can't scale or position (vkms, simple drivers): CPU scaling from now on */
        video_log(&d->base, "hardware scaling unavailable (%s): CPU scaling", strerror(-e));
        if (d->doubled) video_log(&d->base, "av_detail = high needs hardware scaling: av_detail = normal draws as sharp for less work");
        d->hw_scale_broken = true;
        if (e == -EACCES) {
          destroy_all(d, nb, &nblack);
          return e;
        }
        /* keep what is on screen (nb black) until the CPU-path set replaces it */
        dbuf_t old[NBUF], oldb = d->black;
        memcpy(old, d->buf, sizeof old);
        memcpy(d->buf, nb, sizeof nb);
        d->black = nblack;
        destroy_all(d, old, &oldb);
        continue;
      }
    }
    /* success: the new set is on screen, the old one can go */
    destroy_all(d, d->buf, &d->black);
    memcpy(d->buf, nb, sizeof nb);
    d->black = nblack;
    if (d->steal.fb_id) buf_destroy(d->fd, &d->steal);
    free(d->xlut);
    free(d->ylut);
    d->xlut = d->ylut = NULL;
    d->hw_scale = hw;
    d->cpu_scale = !hw && !exact;
    if (d->cpu_scale) {
      if (lay.view_w != d->rw) d->xlut = malloc(sizeof(uint16_t) * (size_t)lay.view_w);
      if (lay.view_h != d->rh) d->ylut = malloc(sizeof(uint16_t) * (size_t)lay.view_h);
      if ((lay.view_w != d->rw && !d->xlut) || (lay.view_h != d->rh && !d->ylut)) return -ENOMEM;
      video_scale_luts(d->rw, d->rh, lay.view_w, lay.view_h, d->xlut, d->ylut);
    }
    d->lay = lay;
    d->plane_id = plane;
    d->mode = *m;
    d->crtc_id = crtc_id;
    d->crtc_idx = crtc_idx;
    d->front = 0;
    d->pending = -1;
    d->gen++;
    d->active = true;
    if (hw) set_scaling_filter(d);
    return 0;
  }
  return -EINVAL;
}

/* composite: the TV norm through the connector's "TV mode" property, ordered so every commit
 * passes the VEC's (norm, htotal) check [4.8]; then the mode */
static int apply_composite(drm_t *d, const video_mode_t *m, const conn_t *c, uint32_t crtc_id, uint32_t crtc_idx) {
  const char *norm = video_tv_norm(d->tv);
  const char *prop = "TV mode";
  uint32_t pid;
  uint64_t cur = 0, want = 0;
  bool have = !find_prop(d->fd, c->id, DRM_MODE_OBJECT_CONNECTOR, prop, &pid, &cur) &&
              !prop_enum_value(d->fd, pid, norm, &want);
  if (!have && !find_prop(d->fd, c->id, DRM_MODE_OBJECT_CONNECTOR, "mode", &pid, &cur) &&
      !prop_enum_value(d->fd, pid, norm, &want)) {
    prop = "mode";                     /* the older vc4-only enum */
    have = true;
  }
  const char *first = norm;
  if (have && cur != want) {
    struct drm_mode_crtc g = { .crtc_id = crtc_id };
    unsigned active_ht = !xioctl(d->fd, DRM_IOCTL_MODE_GETCRTC, &g) && g.mode_valid ? g.mode.htotal : 0;
    if (active_ht && !video_vec_valid(norm, active_ht)) first = "PAL";   /* valid with 858 and 864 */
    int e = set_enum_prop(d->fd, c->id, DRM_MODE_OBJECT_CONNECTOR, prop, first);
    if (e) video_log(&d->base, "%s %s: %s", prop, first, strerror(-e));
  }
  int e = apply_mode(d, m, c, crtc_id, crtc_idx);
  if (!e && have && strcmp(first, norm)) {
    int e2 = set_enum_prop(d->fd, c->id, DRM_MODE_OBJECT_CONNECTOR, prop, norm);
    if (e2) video_log(&d->base, "%s %s: %s", prop, norm, strerror(-e2));
  }
  return e;
}

static void fill_info(drm_t *d) {
  video_info_t *i = &d->base.info;
  plat_strlcpy(i->backend, "drm", sizeof i->backend);
  i->render_w = d->rw;
  i->render_h = d->rh;
  plat_strlcpy(i->device, d->path, sizeof i->device);
  i->composite = d->composite;
  snprintf(i->connector, sizeof i->connector, "%s-%u", conn_type_name(d->conn_type), d->conn_type_id);
  if (!d->active && !d->mode.hdisplay) {
    snprintf(i->desc, sizeof i->desc, "%s: no picture yet (%s), render %dx%d", i->connector, d->down_why, d->rw, d->rh);
    return;
  }
  i->mode_w = d->mode.hdisplay;
  i->mode_h = d->mode.vdisplay;
  i->view_x = d->lay.view_x;
  i->view_y = d->lay.view_y;
  i->view_w = d->lay.view_w;
  i->view_h = d->lay.view_h;
  i->hz = video_mode_hz(&d->mode);
  i->interlaced = mode_il(&d->mode);
  i->hw_scaling = d->hw_scale;
  i->connected = !d->blind;
  char tv[32] = "";
  if (d->composite) {
    char up[16];
    size_t k = 0;
    for (; d->tv[k] && k < sizeof up - 1; k++) up[k] = (char)toupper((unsigned char)d->tv[k]);
    up[k] = 0;
    snprintf(tv, sizeof tv, ", %s %s", up, d->aspect[0] ? d->aspect : "4:3");
  }
  snprintf(i->desc, sizeof i->desc, "%s %dx%d%s %.2f Hz%s%s, render %dx%d%s%s", i->connector, i->mode_w, i->mode_h,
           i->interlaced ? "i" : "", i->hz, tv, d->blind ? " (no display seen)" : "", d->rw, d->rh,
           d->cpu_scale ? ", CPU scaling" : "", d->active ? "" : " (down)");
}

static void log_modes(drm_t *d, const conn_t *c) {
  char line[300], nm[24];
  conn_name(c, nm, sizeof nm);
  int n = snprintf(line, sizeof line, "%s on %s (%s): status %u, %u mode%s:", nm, d->path, d->driver, c->status,
                   c->nmodes, c->nmodes == 1 ? "" : "s");
  for (uint32_t i = 0; i < c->nmodes && i < 12 && n < (int)sizeof line - 32; i++)
    n += snprintf(line + n, sizeof line - (size_t)n, " %ux%u%s@%.2f%s", c->modes[i].hdisplay, c->modes[i].vdisplay,
                  mode_il(&c->modes[i]) ? "i" : "", video_mode_hz(&c->modes[i]), mode_pref(&c->modes[i]) ? "*" : "");
  if (c->nmodes > 12) snprintf(line + n, sizeof line - (size_t)n, " ...");
  video_log(&d->base, "%s", line);
}

/* (Re)initialize everything: card, connector, CRTC, mode, buffers. 0 = picture on. */
static int drm_setup(drm_t *d, int flags, bool first) {
  int e;
  if (d->fd < 0) {
    if ((e = open_card(d, 0))) return e;
    flags |= SETUP_PROBE;
  }
  if ((e = get_resources(d->fd, &d->res))) return e;
  if (!d->master) try_master(d, 0);     /* probing works without, but only cached */
  if (pick_connector(d, (flags & SETUP_PROBE) && d->master) < 0) return -ENODEV;
  const conn_t *c = &d->best;
  int cls = conn_class(c->type);
  int want = d->out == OUT_AV ? CLS_COMPOSITE : d->out == OUT_HDMI ? CLS_HDMI : cls;
  if (cls != want && !d->warned_type) {
    char nm[24];
    conn_name(c, nm, sizeof nm);
    video_log(&d->base, "no %s output on %s: using %s", d->out == OUT_AV ? "composite" : "HDMI", d->path, nm);
    d->warned_type = true;
  }
  if (first) log_modes(d, c);
  cands_t cands = { .n = 0 };
  if (cls == CLS_COMPOSITE) composite_cands(d, c, &cands);
  else hdmi_cands(d, c, &cands);
  if (!d->rw) {
    /* the render size is chosen once, from the mode we aim for; a progressive composite mode with
     * av_detail = high draws twice its lines and the plane scales them down (supersampled) */
    video_layout_t l;
    const video_mode_t *m0 = &cands.m[0];
    d->doubled = cls == CLS_COMPOSITE && d->detail_high && !mode_il(m0) && !d->cfg_rw;
    if (d->doubled)
      video_layout_double(m0->hdisplay, m0->vdisplay, d->aspect, d->margin, &l);
    else
      video_layout(m0->hdisplay, m0->vdisplay, cls == CLS_COMPOSITE, d->aspect, d->margin, d->cfg_rw, d->cfg_rh, &l);
    d->rw = l.render_w;
    d->rh = l.render_h;
  }
  d->conn_id = c->id;
  d->conn_type = c->type;
  d->conn_type_id = c->type_id;
  d->composite = cls == CLS_COMPOSITE;
  d->blind = cls != CLS_COMPOSITE && (c->status != CONN_CONNECTED || !c->nmodes);
  /* connectors whose status we watch for hotplug (not composite: it has none) */
  d->hp_n = 0;
  for (uint32_t i = 0; i < d->res.nconn && d->hp_n < MAXOBJ; i++) {
    if (get_connector(d->fd, d->res.conn[i], false, &d->scan) || conn_class(d->scan.type) == CLS_COMPOSITE) continue;
    d->hp_ids[d->hp_n] = d->scan.id;
    d->hp_status[d->hp_n++] = d->scan.status;
  }
  uint32_t crtc_id, crtc_idx;
  if ((e = pick_crtc(d, c, &crtc_id, &crtc_idx))) return e;
  if (!d->master && (e = try_master(d, 0))) return e;
  if (!d->have_saved && !d->modeset_done) {     /* the console's state, not one of ours */
    d->saved.crtc_id = crtc_id;
    if (!xioctl(d->fd, DRM_IOCTL_MODE_GETCRTC, &d->saved) && d->saved.mode_valid && d->saved.fb_id) {
      d->have_saved = true;
      d->saved_conn = c->id;
    }
  }
  uint32_t old_crtc = d->active ? d->crtc_id : 0;
  if (flags & SETUP_DISABLE) {
    struct drm_mode_crtc off = { .crtc_id = old_crtc ? old_crtc : crtc_id };
    xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &off);
    d->active = false;
    old_crtc = 0;
  }
  e = -EINVAL;
  for (int i = 0; i < cands.n; i++) {
    e = cls == CLS_COMPOSITE ? apply_composite(d, &cands.m[i], c, crtc_id, crtc_idx)
                             : apply_mode(d, &cands.m[i], c, crtc_id, crtc_idx);
    if (!e) break;
    if (e != -EINVAL && e != -ERANGE && e != -ENOSPC) break;    /* not a mode problem */
    video_log(&d->base, "mode %ux%u%s@%.2f refused: %s", cands.m[i].hdisplay, cands.m[i].vdisplay,
              mode_il(&cands.m[i]) ? "i" : "", video_mode_hz(&cands.m[i]), strerror(-e));
  }
  if (e) return e;
  if (old_crtc && old_crtc != d->crtc_id) {
    struct drm_mode_crtc off = { .crtc_id = old_crtc };
    xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &off);
  }
  /* timing restarts with the new mode */
  d->base.period_us = d->mode_period_us = (int64_t)llround(1e6 / video_mode_hz(&d->mode));
  int64_t now = plat_now_us();
  d->base.last_vblank_us = now;
  d->base.phase_us = now;
  d->base.soft_due_us = 0;
  d->have_seq = false;
  fill_info(d);
  return 0;
}

/* ================================================================ recovery */

static void go_down(drm_t *d, const char *why, int e) {
  int64_t now = plat_now_us();
  d->backoff_ms = d->backoff_ms ? (d->backoff_ms * 2 > 10000 ? 10000 : d->backoff_ms * 2) : 1000;
  d->retry_at_us = now + (int64_t)d->backoff_ms * 1000;
  if (!d->base.down || strncmp(d->down_why, why, sizeof d->down_why - 1))
    video_log(&d->base, "no picture: %s%s%s (retrying, back-off up to 10 s)", why, e ? ": " : "", e ? strerror(-e) : "");
  plat_strlcpy(d->down_why, why, sizeof d->down_why);
  d->base.down = true;
  d->pending = -1;
  d->gen++;
  /* the card itself went away (module unloaded, USB display unplugged): reopen next time */
  if (e == -ENODEV || e == -EBADF || e == -EIO || e == -ENXIO) close_card(d);
  fill_info(d);
}

static bool owns_display(drm_t *d);

/* Our VT is not in front. KD_GRAPHICS makes the kernel ignore VT switches, so someone put our VT
 * back into text mode first (an admin, --restore-vt). Don't fight for the display: give the CRTC
 * back to the console as we found it and drop master, so the other VT (fbcon, or a DRM client
 * there) can use the screen. try_up() takes it back when our VT is in front again. */
static void go_background(drm_t *d) {
  if (!d->bg && d->fd >= 0) {
    if (d->active && d->have_saved && d->crtc_id == d->saved.crtc_id && d->conn_id == d->saved_conn &&
        owns_display(d)) {
      struct drm_mode_crtc r = d->saved;
      uint32_t conn = d->saved_conn;
      r.set_connectors_ptr = U64P(&conn);
      r.count_connectors = 1;
      xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &r);
    }
    if (d->master && !xioctl(d->fd, DRM_IOCTL_DROP_MASTER, NULL)) d->master = false;
    d->active = false;
  }
  d->bg = true;
  go_down(d, "our VT is not in front", 0);
}

static void recover(drm_t *d, const char *why, int e) {
  d->base.reinits++;
  d->flip_ok_streak = 0;
  d->pending = -1;
  d->gen++;
  if (!sys_console_foreground()) {
    go_background(d);
    return;
  }
  sys_console_refresh();         /* in front but maybe put into text mode: graphics again */
  if (++d->fails_since_ok > 3) {
    /* re-inits that don't stick (flips fail right after): stop thrashing, back off */
    go_down(d, why, e);
    return;
  }
  video_log(&d->base, "re-init: %s%s%s", why, e ? ": " : "", e ? strerror(-e) : "");
  if (e == -ENODEV || e == -EBADF || e == -EIO || e == -ENXIO) close_card(d);
  if (e == -EACCES) d->master = false;
  int r = drm_setup(d, 0, false);
  if (r) {
    go_down(d, "re-init failed", r);
    return;
  }
  d->base.down = false;
  d->backoff_ms = 0;
}

/* a setup after a down period worked */
static void came_up(drm_t *d) {
  if (d->base.down) video_log(&d->base, "picture back: %s", d->base.info.desc);
  d->base.down = false;
  d->bg = false;
  d->backoff_ms = 0;
  d->fails_since_ok = 0;
}

static void try_up(drm_t *d, int64_t now) {
  if (!sys_console_foreground()) {
    d->retry_at_us = now + 1000000;
    return;
  }
  if (d->bg) sys_console_refresh();   /* back in front: KD_GRAPHICS + K_OFF again */
  d->bg = false;
  int r = drm_setup(d, 0, false);
  if (r) {
    char why[96];
    plat_strlcpy(why, d->down_why, sizeof why);
    go_down(d, why, r);
    return;
  }
  came_up(d);
}

/* ================================================================ events + hotplug */

static int64_t flip_timeout_us(const drm_t *d) {
  int64_t t = 4 * d->base.period_us;
  return t > 100000 ? t : 100000;
}

/* read the flip events that are there (non-blocking); -errno if the device failed */
static int read_events(drm_t *d) {
  uint64_t raw[128];                     /* 1 KiB, 8-byte aligned */
  for (;;) {
    ssize_t n = read(d->fd, raw, sizeof raw);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) return 0;
      return -errno;
    }
    if (n == 0) return 0;
    const uint8_t *p = (const uint8_t *)raw;
    for (ssize_t off = 0; off + (ssize_t)sizeof(struct drm_event) <= n;) {
      struct drm_event h;
      memcpy(&h, p + off, sizeof h);
      if (h.length < sizeof h || off + (ssize_t)h.length > n) break;
      if (h.type == DRM_EVENT_FLIP_COMPLETE && h.length >= sizeof(struct drm_event_vblank)) {
        struct drm_event_vblank vb;
        memcpy(&vb, p + off, sizeof vb);
        uint32_t gen = (uint32_t)(vb.user_data >> 32), idx = (uint32_t)vb.user_data;
        if (gen == d->gen && (int)idx == d->pending) {
          int64_t arrival = plat_now_us();
          int64_t ts = (int64_t)vb.tv_sec * 1000000 + vb.tv_usec;
          if (!d->ts_mono) {            /* CLOCK_REALTIME stamps (old kernels): move to monotonic */
            struct timespec rt;
            clock_gettime(CLOCK_REALTIME, &rt);
            ts += arrival - ((int64_t)rt.tv_sec * 1000000 + rt.tv_nsec / 1000);
          }
          if (ts > arrival + 50000 || ts < arrival - 1000000) ts = arrival;   /* nonsense stamp */
          /* refreshes since the last flip, counted by time (like video_fb.c): the vblank counter
           * can run faster than the refresh. On a Pi 4's composite 576i it counts 2 per 20 ms
           * refresh, so every frame looked late (period estimate 10 ms, margin and pacer budget
           * at their maximum, frames started ~10 ms too early). */
          int64_t dt = ts - d->base.last_vblank_us, nom = d->mode_period_us;
          int64_t nv = d->have_seq && dt > 0 && nom > 0 ? (dt + nom / 2) / nom : 0;
          if (d->have_seq && dt > 0 && nv < 1) nv = 1;
          if (nv >= 1000) nv = 0;
          if (nv >= 1) {
            int64_t per = dt / nv;
            if (per > nom * 9 / 10 && per < nom * 11 / 10)
              d->base.period_us = (d->base.period_us * 15 + per) / 16;
            /* adaptive margin [5.3]: missed although queued in time -> more margin */
            int64_t T = d->base.period_us;
            if (nv > 1 && d->flip_on_time) {
              d->base.margin_us += 250;
              d->good_flips = 0;
            } else if (nv == 1 && ++d->good_flips >= 100) {
              d->base.margin_us -= 50;
              d->good_flips = 0;
            }
            if (d->base.margin_us < 500) d->base.margin_us = 500;
            if (d->base.margin_us > T / 2) d->base.margin_us = T / 2;
          }
          video_note_shown(&d->base, ts, arrival, (uint32_t)nv);
          d->have_seq = true;
          d->front = (int)idx;
          d->pending = -1;
          if (++d->flip_ok_streak >= 60) d->fails_since_ok = 0;
        }
      }
      off += h.length;
    }
  }
}

/* drain the uevent socket; true if a DRM hotplug event (or an overflow) was seen */
static bool hotplug_read(int s) {
  char m[4096];
  bool hit = false;
  for (;;) {
    ssize_t n = recv(s, m, sizeof m - 1, MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == ENOBUFS) { hit = true; continue; }   /* lost some: probe to be sure */
      break;
    }
    if (n == 0) break;
    m[n] = 0;
    bool drm = false, hp = false;
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(m + i) + 1) {
      if (!strcmp(m + i, "SUBSYSTEM=drm")) drm = true;
      else if (!strcmp(m + i, "HOTPLUG=1")) hp = true;
    }
    hit |= drm && hp;
  }
  return hit;
}

static int hotplug_open(void) {
  int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
  if (s < 0) return -1;
  struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };
  if (bind(s, (struct sockaddr *)&sa, sizeof sa)) {
    close(s);
    return -1;
  }
  return s;
}

static void *probe_main(void *arg) {
  drm_t *d = arg;
  plat_thread_lower_priority();
  /* the forced probe reads EDID over DDC (tens of ms): not on the frame loop [7] */
  for (uint32_t i = 0; i < d->probe_n; i++) {
    struct drm_mode_get_connector g = { .connector_id = d->probe_ids[i] };
    xioctl(d->probe_fd, DRM_IOCTL_MODE_GETCONNECTOR, &g);
  }
  __atomic_store_n(&d->probe_state, 2, __ATOMIC_SEQ_CST);
  return NULL;
}

static void start_probe(drm_t *d) {
  if (d->fd < 0 || !d->hp_n || __atomic_load_n(&d->probe_state, __ATOMIC_SEQ_CST)) return;
  d->probe_fd = d->fd;
  d->probe_n = d->hp_n;
  memcpy(d->probe_ids, d->hp_ids, sizeof d->hp_ids[0] * d->hp_n);
  __atomic_store_n(&d->probe_state, 1, __ATOMIC_SEQ_CST);
  if (plat_thread_start(&d->probe_th, probe_main, d, false) != 0) {
    __atomic_store_n(&d->probe_state, 0, __ATOMIC_SEQ_CST);
    probe_main(d);                     /* no thread: probe inline (rare) */
    __atomic_store_n(&d->probe_state, 0, __ATOMIC_SEQ_CST);
    d->probe_n = 0;
  }
}

/* after a probe: does the best output or its mode differ from what is on? */
static void after_probe(drm_t *d) {
  if (d->bg || !sys_console_foreground()) return;       /* try_up re-checks everything later */
  if (d->fd < 0 || get_resources(d->fd, &d->res) || pick_connector(d, false) < 0) return;
  const conn_t *c = &d->best;
  for (uint32_t i = 0; i < d->hp_n; i++)
    if (d->hp_ids[i] == c->id) d->hp_status[i] = c->status;
  bool connected = c->status == CONN_CONNECTED && c->nmodes;
  if (conn_class(c->type) == CLS_COMPOSITE) return;
  cands_t cands = { .n = 0 };
  hdmi_cands(d, c, &cands);
  bool change = c->id != d->conn_id || (d->blind && connected) || !d->active ||
                (cands.n && !video_same_timing(&cands.m[0], &d->mode));
  char nm[24];
  conn_name(c, nm, sizeof nm);
  if (change) {
    video_log(&d->base, "hotplug: %s %s", nm, connected ? "connected" : "changed");
    d->base.reinits++;
    d->fails_since_ok = 0;
    int r = drm_setup(d, SETUP_DISABLE, false);  /* off first: the re-enable sends HDMI infoframes */
    if (r) go_down(d, "after hotplug", r);
    else came_up(d);
  } else if (!connected && !d->blind) {
    video_log(&d->base, "hotplug: %s disconnected (keeps running blind)", nm);
    d->blind = true;
    fill_info(d);
  }
}

static bool owns_display(drm_t *d) {
  struct drm_mode_crtc c = { .crtc_id = d->crtc_id };
  if (xioctl(d->fd, DRM_IOCTL_MODE_GETCRTC, &c) || !c.mode_valid) return false;
  if (c.fb_id && c.fb_id == d->black.fb_id) return true;
  for (int i = 0; i < NBUF; i++)
    if (c.fb_id && c.fb_id == d->buf[i].fb_id) return true;
  return false;
}

/* everything that isn't the flip itself: events, hotplug, the 1 Hz checks, retries */
static void service(drm_t *d, int64_t now) {
  if (d->fd >= 0 && d->pending >= 0) {
    int e = read_events(d);
    if (e) go_down(d, "reading events", e);
  }
  if (__atomic_load_n(&d->probe_state, __ATOMIC_SEQ_CST) == 2) {
    probe_join(d);
    after_probe(d);
  }
  if (d->nl_fd >= 0 && now >= d->next_nl_us) {
    d->next_nl_us = now + 100000;
    if (hotplug_read(d->nl_fd)) d->hotplug = true;
  }
  if (d->hotplug && d->fd >= 0 && !__atomic_load_n(&d->probe_state, __ATOMIC_SEQ_CST)) {
    d->hotplug = false;
    start_probe(d);
  }
  if (now >= d->next_check_us) {
    d->next_check_us = now + 1000000;
    if (!d->base.down && d->fd >= 0) {
      if (!sys_console_foreground()) {
        go_background(d);
      } else if (d->active && d->pending < 0 && !owns_display(d)) {
        recover(d, "another client took the display", 0);
      }
    }
    /* backup for missed uevents: cached connector status (the HPD IRQ keeps it fresh); also
     * while down, so a display plugged in later is used at once instead of at the next retry */
    for (uint32_t i = 0; i < d->hp_n && d->fd >= 0 && !d->bg; i++)
      if (!get_connector(d->fd, d->hp_ids[i], false, &d->scan) && d->scan.status != d->hp_status[i]) {
        d->hp_status[i] = d->scan.status;
        d->hotplug = true;
      }
  }
  if (!d->base.down && d->pending >= 0 && now - d->flip_issued_us > flip_timeout_us(d))
    recover(d, "no flip event", 0);
  if (d->base.down && now >= d->retry_at_us) try_up(d, now);
}

/* ================================================================ ops */

static void copy_frame(drm_t *d, int b, const uint16_t *fb) {
  dbuf_t *B = &d->buf[b];
  if (!B->map) return;
  if (d->cpu_scale)
    video_scale_copy(fb, d->rw, B->map, (int)B->pitch, d->bpp, d->lay.view_x, d->lay.view_y, d->lay.view_w,
                     d->lay.view_h, d->xlut, d->ylut);
  else if (d->bpp == 16 && B->pitch == (uint32_t)d->rw * 2)
    memcpy(B->map, fb, (size_t)d->rw * (size_t)d->rh * 2);
  else
    video_scale_copy(fb, d->rw, B->map, (int)B->pitch, d->bpp, 0, 0, d->rw, d->rh, NULL, NULL);
}

static int flip(drm_t *d, int b) {
  if (d->force_flip_err) {
    int e = d->force_flip_err;
    d->force_flip_err = 0;
    return e;
  }
  struct drm_mode_crtc_page_flip f = { .crtc_id = d->crtc_id, .fb_id = d->buf[b].fb_id,
                                       .flags = DRM_MODE_PAGE_FLIP_EVENT, .reserved = 0,
                                       .user_data = ((uint64_t)d->gen << 32) | (uint32_t)b };
  int e = xioctl(d->fd, DRM_IOCTL_MODE_PAGE_FLIP, &f);
  if (!e && d->lose_event) {
    d->lose_event = false;
    d->gen++;                      /* its event is now "stale": as if it never came */
  }
  if (!e) {
    int64_t now = plat_now_us();
    d->pending = b;
    d->flip_issued_us = now;
    /* in time for the vblank after the last event? (for the adaptive margin) */
    d->flip_on_time = now - d->base.phase_us < d->base.period_us &&
                      now <= d->base.phase_us + d->base.period_us - d->base.margin_us;
  }
  return e;
}

/* wait for the pending flip's event up to timeout_ms; 0 done (or recovered), 1 not yet */
static int wait_flip(drm_t *d, int timeout_ms) {
  int64_t end = plat_now_us() + (int64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000;
  for (;;) {
    int e = read_events(d);
    if (e) {
      go_down(d, "reading events", e);
      return 0;
    }
    if (d->pending < 0) return 0;
    int64_t now = plat_now_us(), lost = d->flip_issued_us + flip_timeout_us(d);
    if (now >= lost) {
      recover(d, "no flip event", 0);
      return 0;
    }
    if (now >= end) return 1;
    int64_t until = end < lost ? end : lost, us = until - now;
    struct timespec ts = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };
    struct pollfd p = { .fd = d->fd, .events = POLLIN };
    int r = ppoll(&p, 1, &ts, NULL);
    if (r < 0 && errno == EINTR) return 1;     /* a signal: let the loop look at its quit flag */
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      go_down(d, "device error", -ENODEV);
      return 0;
    }
  }
}

/* let a pending flip land, up to ms, without any recovery (close, test hooks): a lost event
 * must not start a modeset while we are shutting down */
static void settle_flip(drm_t *d, int ms) {
  int64_t end = plat_now_us() + (int64_t)ms * 1000;
  while (d->fd >= 0 && d->pending >= 0) {
    if (read_events(d) || d->pending < 0) break;
    int64_t us = end - plat_now_us();
    if (us <= 0) break;
    struct timespec ts = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };
    struct pollfd p = { .fd = d->fd, .events = POLLIN };
    int r = ppoll(&p, 1, &ts, NULL);
    if (r < 0 && errno != EINTR) break;
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
  }
  if (d->pending >= 0) {         /* never came: forget it (its event is stale from now on) */
    d->pending = -1;
    d->gen++;
  }
}

static int drm_present(video_t *v, const uint16_t *fb) {
  drm_t *d = (drm_t *)v;
  service(d, plat_now_us());
  if (!d->base.down && d->pending >= 0) wait_flip(d, (int)(flip_timeout_us(d) / 1000) + 1);
  if (!d->base.down && d->pending < 0 && d->active) {
    for (int tries = 0; tries < 2; tries++) {
      int b = d->front == 0 ? 1 : 0;       /* the one not on screen */
      copy_frame(d, b, fb);
      int e = flip(d, b);
      if (!e) return 0;
      /* EBUSY with nothing pending = the plane lost its fb; EINVAL = CRTC off or format/size
       * changed by someone else; ENOSPC; ENOENT; EACCES = not master: all -> full re-init */
      char why[48];
      snprintf(why, sizeof why, "page flip failed");
      recover(d, why, e);
      if (d->base.down || !d->active) break;
    }
  }
  if (d->base.down || !d->active) {
    /* no display: keep the loop paced like a 60 Hz screen */
    if (d->base.soft_due_us) video_soft_wait(v, 1000, NULL);
    video_soft_queue(v, plat_now_us());
  }
  return 0;
}

static int drm_wait(video_t *v, int timeout_ms, int64_t *t_us) {
  drm_t *d = (drm_t *)v;
  service(d, plat_now_us());
  if (d->base.down || !d->active) return video_soft_wait(v, timeout_ms, t_us);
  int r = d->pending >= 0 ? wait_flip(d, timeout_ms) : 0;
  if (t_us) *t_us = d->base.last_vblank_us;
  return r;
}

static bool drm_pending(const video_t *v) {
  const drm_t *d = (const drm_t *)v;
  if (d->base.down || !d->active) return video_soft_pending(v);
  return d->pending >= 0;
}

static int drm_fd(const video_t *v) {
  const drm_t *d = (const drm_t *)v;
  return d->base.down || !d->active ? -1 : d->fd;
}

static void drm_close(video_t *v) {
  drm_t *d = (drm_t *)v;
  if (d->fd >= 0) {
    settle_flip(d, 100);                         /* let a pending flip land [12] */
    probe_join(d);
    /* Still on screen: no VT handed the display back (dev runs without a VT), or the caller
     * closes the video before restoring the console. Put the console's CRTC state back, so
     * removing our buffers doesn't switch the CRTC off (a black flash and, on composite, a
     * lost signal until fbcon's own restore). After a KD_TEXT fbcon owns it: nothing to do. */
    if (d->have_saved && d->active && d->crtc_id == d->saved.crtc_id && d->conn_id == d->saved_conn &&
        owns_display(d)) {
      struct drm_mode_crtc r = d->saved;
      uint32_t conn = d->saved_conn;
      r.set_connectors_ptr = U64P(&conn);
      r.count_connectors = 1;
      xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &r);
    }
  }
  close_card(d);
  if (d->nl_fd >= 0) close(d->nl_fd);
  free(d->xlut);
  free(d->ylut);
  free(d);
}

static int drm_debug(video_t *v, const char *a) {
  drm_t *d = (drm_t *)v;
  if (!strcmp(a, "reinit")) {
    d->fails_since_ok = 0;
    recover(d, "requested", 0);
    return 0;
  }
  if (!strcmp(a, "hotplug")) {
    d->hotplug = true;
    if (!d->hp_n && d->fd >= 0) {              /* no hotplug-capable connector: just re-check ours */
      d->hp_ids[0] = d->conn_id;
      d->hp_status[0] = 0;
      d->hp_n = 1;
    }
    return 0;
  }
  if (!strcmp(a, "flipfail")) {
    d->force_flip_err = -EINVAL;
    return 0;
  }
  if (!strcmp(a, "loseevent")) {                /* the next flip's event goes missing: timeout path */
    d->lose_event = true;
    return 0;
  }
  if (d->fd < 0 || !d->active) return -1;
  if (!strcmp(a, "rmfb")) {                     /* the front buffer vanishes under us */
    int f = d->front >= 0 ? d->front : 0;
    uint32_t id = d->buf[f].fb_id;
    if (xioctl(d->fd, DRM_IOCTL_MODE_RMFB, &id)) return -1;
    /* forget the id: the kernel hands it out again (lowest free), and removing it a second time
     * at the next cleanup would remove a new buffer instead */
    d->buf[f].fb_id = 0;
    return 0;
  }
  if (!strcmp(a, "disable")) {                  /* the CRTC goes off */
    struct drm_mode_crtc off = { .crtc_id = d->crtc_id };
    return xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &off) ? -1 : 0;
  }
  if (!strcmp(a, "dropmaster")) return xioctl(d->fd, DRM_IOCTL_DROP_MASTER, NULL) ? -1 : 0;
  if (!strcmp(a, "steal")) {                    /* like fbcon's forced restore: a foreign fb */
    settle_flip(d, 100);
    buf_destroy(d->fd, &d->steal);
    if (buf_create(d, &d->steal, d->mode.hdisplay, d->mode.vdisplay)) return -1;
    uint32_t conn = d->conn_id;
    struct drm_mode_crtc sc = { .set_connectors_ptr = U64P(&conn), .count_connectors = 1, .crtc_id = d->crtc_id,
                                .fb_id = d->steal.fb_id, .mode_valid = 1 };
    memcpy(&sc.mode, &d->mode, sizeof sc.mode);
    return xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &sc) ? -1 : 0;
  }
  return -1;
}

static const video_ops_t drm_ops = { drm_present, drm_wait, drm_pending, drm_fd, drm_close, drm_debug };

static void cfg_str(char *dst, size_t n, const char *src, const char *def) {
  plat_strlcpy(dst, src && *src ? src : def, n);
  for (char *p = dst; *p; p++) *p = (char)tolower((unsigned char)*p);
}

video_t *video_drm_open(const video_config_t *c, video_info_t *info, char *err, int errlen) {
  static const video_config_t defaults = { 0 };
  if (!c) c = &defaults;
  drm_t *d = calloc(1, sizeof *d);
  if (!d) {
    if (err && errlen > 0) snprintf(err, (size_t)errlen, "out of memory");
    return NULL;
  }
  video_base_init(&d->base, &drm_ops, c, 60.0);
  d->fd = d->nl_fd = -1;
  d->front = d->pending = -1;
  d->bpp = 16;
  d->base.margin_us = 1000;
  /* the device setting is shared with fb: a /dev/fbN path means "find the KMS card" here */
  if (c->device && *c->device && strncmp(c->device, "/dev/fb", 7))
    plat_strlcpy(d->device, c->device, sizeof d->device);
  cfg_str(d->tv, sizeof d->tv, c->tv, "ntsc");
  cfg_str(d->aspect, sizeof d->aspect, c->aspect, "4:3");
  cfg_str(d->hdmi_mode, sizeof d->hdmi_mode, c->hdmi_mode, "auto");
  char tmp[16];
  cfg_str(tmp, sizeof tmp, c->output, "auto");
  d->out = !strcmp(tmp, "av") ? OUT_AV : !strcmp(tmp, "hdmi") ? OUT_HDMI : OUT_AUTO;
  cfg_str(tmp, sizeof tmp, c->av_mode, "progressive");
  d->progressive = !strcmp(tmp, "progressive") || !strcmp(tmp, "240p") || !strcmp(tmp, "288p");
  cfg_str(tmp, sizeof tmp, c->av_detail, "high");
  d->detail_high = !strcmp(tmp, "high");
  if (c->scale_filter && *c->scale_filter) {
    cfg_str(tmp, sizeof tmp, c->scale_filter, "smooth");
    d->filter_set = true;
    d->sharp = !strcmp(tmp, "sharp") || !strcmp(tmp, "nearest");
  }
  d->margin = c->margin_pct;
  if (c->render_w > 0 && c->render_h > 0 && c->render_w <= 8192 && c->render_h <= 8192) {
    d->cfg_rw = c->render_w;
    d->cfg_rh = c->render_h;
  }
  int wait = c->card_wait_ms < 0 ? 0 : c->card_wait_ms > 0 ? c->card_wait_ms : (uptime_s() < 60 ? 15000 : 0);
  int e = open_card(d, wait);
  if (e) {
    if (err && errlen > 0)
      snprintf(err, (size_t)errlen, "no KMS device%s%s (%s)", d->device[0] ? " at " : "", d->device, strerror(-e));
    free(d);
    return NULL;
  }
  try_master(d, 3000);       /* a stray instance may still be exiting [3] */
  e = drm_setup(d, SETUP_PROBE, true);
  if (e == -ENODEV && !d->rw) {
    if (err && errlen > 0) snprintf(err, (size_t)errlen, "%s: no usable connector", d->path);
    close_card(d);
    free(d);
    return NULL;
  }
  if (e) {
    /* the card is there but no picture yet (another master, refused modes, VT elsewhere):
     * keep the loop running and retry with back-off */
    if (!d->rw) {
      video_layout_t l;
      bool av = d->out != OUT_HDMI, p625 = video_tv_625(d->tv);
      int mh = d->progressive ? (p625 ? 288 : 240) : (p625 ? 576 : 480);
      d->doubled = av && d->progressive && d->detail_high && !d->cfg_rw;
      if (d->doubled) video_layout_double(720, mh, d->aspect, d->margin, &l);
      else if (av) video_layout(720, mh, true, d->aspect, d->margin, d->cfg_rw, d->cfg_rh, &l);
      else video_layout(1280, 720, false, d->aspect, d->margin, d->cfg_rw, d->cfg_rh, &l);
      d->rw = l.render_w;
      d->rh = l.render_h;
    }
    go_down(d, "start", e);
  }
  if (!d->composite) d->nl_fd = hotplug_open();
  d->next_check_us = plat_now_us() + 1000000;
  fill_info(d);
  video_log(&d->base, "%s", d->base.info.desc);
  if (info) *info = d->base.info;
  return &d->base;
}

#endif
