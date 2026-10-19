/* sticktime-gpu: the GPU helper of drawing = gpu (protocol: gpu_proto.h). It draws each frame's recorded
 * calls (gpu_geom.c) with OpenGL ES 2 straight into the display's buffers, which the host hands over
 * as dma-bufs. ES 2 runs on every Pi's GPU (VideoCore IV: Pi 3 and Zero 2; VI: Pi 4; VII: Pi 5).
 * Mesa's EGL and GLES are loaded at run time, so the program itself needs only libc; a separate
 * program, so the host stays one static file and a GPU driver crash costs only GPU drawing.
 *
 *   sticktime-gpu            run for the host (socket fd 3, shared memory fd 4)
 *   sticktime-gpu --test     draw test frames into a buffer of its own: what works, how fast
 * With STP_GPU_PROF set (in the host's environment) it logs where a frame's time goes every 120 frames
 * (the GPU then finishes the scene before the filter starts, to tell them apart). */
#define _GNU_SOURCE
#include "gpu_geom.h"
#include "gpu_proto.h"

#include <stdio.h>

#ifndef __linux__
int main(void) {
  fprintf(stderr, "sticktime-gpu: Linux only\n");
  return 1;
}
#else

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

/* ---------------------------------------------------------------- EGL / GLES 2, loaded at run time */
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface, *EGLImage, *EGLClientBuffer;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean, EGLenum;
typedef intptr_t EGLAttrib;
typedef unsigned int GLuint, GLenum, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte, GLboolean;
typedef char GLchar;
typedef intptr_t GLsizeiptr;

#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_SURFACE_TYPE 0x3033
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274

#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_BLEND 0x0BE2
#define GL_ONE 1
#define GL_ZERO 0
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_RENDERER 0x1F01
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_RGBA 0x1908
#define GL_RGB 0x1907
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_LINEAR 0x2601
#define GL_ALPHA 0x1906
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_NO_ERROR 0
#define GL_DITHER 0x0BD0

#define GLFN(X) \
  X(void, glGenFramebuffers, (GLsizei, GLuint *)) X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
  X(void, glBindFramebuffer, (GLenum, GLuint)) X(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
  X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)) X(void, glBindRenderbuffer, (GLenum, GLuint)) \
  X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
  X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) X(GLenum, glCheckFramebufferStatus, (GLenum)) \
  X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) X(void, glFinish, (void)) X(const GLubyte *, glGetString, (GLenum)) \
  X(GLuint, glCreateShader, (GLenum)) X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
  X(void, glCompileShader, (GLuint)) X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
  X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) X(GLuint, glCreateProgram, (void)) \
  X(void, glAttachShader, (GLuint, GLuint)) X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
  X(void, glLinkProgram, (GLuint)) X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) X(void, glUseProgram, (GLuint)) \
  X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) X(void, glUniform1i, (GLint, GLint)) \
  X(void, glUniform1f, (GLint, GLfloat)) X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
  X(void, glGenBuffers, (GLsizei, GLuint *)) X(void, glBindBuffer, (GLenum, GLuint)) \
  X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) X(void, glEnableVertexAttribArray, (GLuint)) \
  X(void, glDisableVertexAttribArray, (GLuint)) \
  X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
  X(void, glDrawArrays, (GLenum, GLint, GLsizei)) X(void, glEnable, (GLenum)) X(void, glDisable, (GLenum)) \
  X(void, glBlendFunc, (GLenum, GLenum)) X(GLenum, glGetError, (void)) X(void, glGenTextures, (GLsizei, GLuint *)) \
  X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) X(void, glClear, (GLbitfield)) \
  X(void, glDeleteTextures, (GLsizei, const GLuint *)) X(void, glBindTexture, (GLenum, GLuint)) \
  X(void, glActiveTexture, (GLenum)) X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
  X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
  X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
  X(void, glPixelStorei, (GLenum, GLint)) X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
#define EGLFN(X) \
  X(EGLDisplay, eglGetPlatformDisplay, (EGLenum, void *, const EGLAttrib *)) \
  X(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *)) X(EGLBoolean, eglBindAPI, (EGLenum)) \
  X(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)) \
  X(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *)) \
  X(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext)) \
  X(EGLImage, eglCreateImage, (EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib *)) \
  X(EGLBoolean, eglDestroyImage, (EGLDisplay, EGLImage)) X(EGLint, eglGetError, (void)) \
  X(void *, eglGetProcAddress, (const char *))
#define DECL(ret, name, args) static ret(*name) args;
GLFN(DECL)
EGLFN(DECL)
static void (*glEGLImageTargetRenderbufferStorageOES)(GLenum, void *);

static char err[160];

static int load_gl(void) {
  void *egl = dlopen("libEGL.so.1", RTLD_NOW), *gles = dlopen("libGLESv2.so.2", RTLD_NOW);
  if (!egl || !gles) {
    snprintf(err, sizeof err, "no EGL / GLES 2 (%s)", dlerror());
    return -1;
  }
#define LOADE(ret, name, args) if (!(name = (ret(*) args)dlsym(egl, #name))) { snprintf(err, sizeof err, "no %s", #name); return -1; }
#define LOADG(ret, name, args) if (!(name = (ret(*) args)dlsym(gles, #name))) { snprintf(err, sizeof err, "no %s", #name); return -1; }
  EGLFN(LOADE)
  GLFN(LOADG)
  glEGLImageTargetRenderbufferStorageOES = (void (*)(GLenum, void *))eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
  if (!glEGLImageTargetRenderbufferStorageOES) {
    snprintf(err, sizeof err, "no GL_OES_EGL_image");
    return -1;
  }
  return 0;
}

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---------------------------------------------------------------- state */
typedef struct {
  int fd;
  EGLImage img;
  GLuint rb, fbo;
  uint8_t *map;                    /* the buffer mapped, when the GPU can't draw into it (glReadPixels) */
  size_t map_len;
} target_t;

static EGLDisplay dpy;
static EGLContext ctx;
static uint8_t *shm;
static target_t tg[GPU_MAX_BUF];
static gpu_msg_t tgt;              /* the TARGET in force (nbuf 0: none) */
static bool direct;                /* the GPU draws into the buffers themselves (else glReadPixels) */
static GLuint scene_tex, scene_fbo, out_tex, out_fbo, atlas_tex, vbo;
static GLuint prog_scene, prog_field;
static GLint u_scene_s, u_scene_asz, u_scene_atlas;
static GLint u_field_t, u_field_tsz, u_field_rows, u_field_pad, u_field_e, u_field_m, u_field_div, u_field_hl, u_field_tt;
static gmesh_t mesh;
static bool prof;
static double prof_t[5];                        /* -, build + issue, scene, out, all (ms) */
static long prof_n, prof_verts, prof_calls;
static uint8_t *rgba_tmp;
static size_t rgba_tmp_n;

static const char *VS_SCENE =
  "attribute vec2 p; attribute vec2 t; attribute vec4 c; uniform vec2 s;\n"
  "varying highp vec2 vt; varying lowp vec4 vc;\n"
  "void main() { vt = t; vc = c; gl_Position = vec4(p.x * s.x - 1.0, p.y * s.y - 1.0, 0.0, 1.0); }\n";
/* a glyph's alpha: lcd.c draws 250 and more as the color itself */
static const char *FS_SCENE =
  "precision highp float; uniform sampler2D atlas; uniform vec2 asz;\n"
  "varying highp vec2 vt; varying lowp vec4 vc;\n"
  "void main() {\n"
  "  float a = 1.0;\n"
  "  if (vt.x >= 0.0) { a = texture2D(atlas, vt / asz).a; if (a >= 0.979) a = 1.0; }\n"
  "  gl_FragColor = vec4(vc.rgb, vc.a * a);\n"
  "}\n";
static const char *VS_FULL =
  "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
/* video_deflicker_lines on the GPU: picture line y = output line - pad is the sum of `rows` render
 * lines, blended e, m, e with its neighbours, divided by (2e + m) rows; lines past the picture are
 * black with pad, else the edge line. Inside the picture the texture's linear filter does the adding:
 * a sample between two rows is their average (a doubled frame's line pair), and two samples tt rows
 * either side of a line's center are [e m e] / (2e + m) with tt = 2e / (2e + m): 2 or 3 reads a pixel
 * instead of 3 or 6. At the edges each row is read at its center (that is the row itself). */
static const char *FS_FIELD =
  "precision highp float; uniform sampler2D t; uniform vec2 tsz;\n"
  "uniform float rows, pad, e, m, dv, hl, tt;\n"
  "vec3 at(float row) { return texture2D(t, vec2(gl_FragCoord.x / tsz.x, row / tsz.y)).rgb; }\n"
  "vec3 pic(float y) {\n"
  "  float k = 1.0;\n"
  "  if (pad > 0.5 && (y < 0.0 || y >= hl)) k = 0.0;\n"
  "  y = clamp(y, 0.0, hl - 1.0);\n"
  "  vec3 s = at(y * rows + 0.5);\n"
  "  if (rows > 1.5) s += at(y * rows + 1.5);\n"
  "  return s * k;\n"
  "}\n"
  "void main() {\n"
  "  float y = floor(gl_FragCoord.y) - pad;\n"
  "  if (y >= 1.0 && y <= hl - 2.0) {\n"
  "    if (rows > 1.5) gl_FragColor = vec4((e * (at(2.0 * y - 1.0) + at(2.0 * y + 3.0)) + m * at(2.0 * y + 1.0)) / (2.0 * e + m), 1.0);\n"
  "    else gl_FragColor = vec4(0.5 * (at(y + 0.5 - tt) + at(y + 0.5 + tt)), 1.0);\n"
  "  } else {\n"
  "    gl_FragColor = vec4((e * (pic(y - 1.0) + pic(y + 1.0)) + m * pic(y)) / dv, 1.0);\n"
  "  }\n"
  "}\n";

static GLuint shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[200] = "";
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    snprintf(err, sizeof err, "shader: %s", log);
    return 0;
  }
  return s;
}

static GLuint program(const char *vs, const char *fs, const char *const *attrs, int nattr) {
  GLuint v = shader(GL_VERTEX_SHADER, vs), f = v ? shader(GL_FRAGMENT_SHADER, fs) : 0;
  if (!f) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  for (int i = 0; i < nattr; i++) glBindAttribLocation(p, (GLuint)i, attrs[i]);
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    snprintf(err, sizeof err, "the shaders don't link");
    return 0;
  }
  return p;
}

static void tex_params(void) {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* a texture of w x h with a framebuffer on it: RGB565 as the CPU draws (half the memory to move), else
 * RGBA8; linear: the filter samples between rows */
static int tex_fbo(GLuint *tex, GLuint *fbo, int w, int h, bool linear) {
  for (int k = 0; k < 2; k++) {
    if (*fbo) glDeleteFramebuffers(1, fbo);
    if (*tex) glDeleteTextures(1, tex);
    *fbo = *tex = 0;
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    tex_params();
    if (linear) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    if (k == 0) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
      glClearColor(0, 0, 0, 1);                  /* a frame that doesn't start with a clear draws on black */
      glClear(GL_COLOR_BUFFER_BIT);
      glGetError();
      return 0;
    }
  }
  snprintf(err, sizeof err, "no %dx%d framebuffer", w, h);
  return -1;
}

static int gl_init(char *renderer, size_t n) {
  if (load_gl()) return -1;
  dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
  EGLint maj = 0, min = 0;
  if (!dpy || !eglInitialize(dpy, &maj, &min)) {
    snprintf(err, sizeof err, "EGL: no display (0x%x)", eglGetError());
    return -1;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
  EGLConfig cfg;
  EGLint ncfg = 0;
  if (!eglChooseConfig(dpy, ca, &cfg, 1, &ncfg) || ncfg < 1) {
    snprintf(err, sizeof err, "EGL: no GLES 2 config (0x%x)", eglGetError());
    return -1;
  }
  EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  ctx = eglCreateContext(dpy, cfg, NULL, xa);
  if (!ctx || !eglMakeCurrent(dpy, NULL, NULL, ctx)) {
    snprintf(err, sizeof err, "EGL: no context (0x%x)", eglGetError());
    return -1;
  }
  snprintf(renderer, n, "%s", (const char *)glGetString(GL_RENDERER));
  if (strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe") || strstr(renderer, "Software")) {
    snprintf(err, sizeof err, "no GPU driver (only %s, on the CPU)", renderer);
    return -1;
  }
  static const char *const sa[] = { "p", "t", "c" }, *const fa[] = { "p" };
  if (!(prog_scene = program(VS_SCENE, FS_SCENE, sa, 3)) || !(prog_field = program(VS_FULL, FS_FIELD, fa, 1))) return -1;
  u_scene_s = glGetUniformLocation(prog_scene, "s");
  u_scene_asz = glGetUniformLocation(prog_scene, "asz");
  u_scene_atlas = glGetUniformLocation(prog_scene, "atlas");
  u_field_t = glGetUniformLocation(prog_field, "t");
  u_field_tsz = glGetUniformLocation(prog_field, "tsz");
  u_field_rows = glGetUniformLocation(prog_field, "rows");
  u_field_pad = glGetUniformLocation(prog_field, "pad");
  u_field_e = glGetUniformLocation(prog_field, "e");
  u_field_m = glGetUniformLocation(prog_field, "m");
  u_field_div = glGetUniformLocation(prog_field, "dv");
  u_field_hl = glGetUniformLocation(prog_field, "hl");
  u_field_tt = glGetUniformLocation(prog_field, "tt");
  glGenTextures(1, &atlas_tex);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, atlas_tex);
  tex_params();
  glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, GPU_ATLAS_W, GPU_ATLAS_MAXH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
  glActiveTexture(GL_TEXTURE0);
  glGenBuffers(1, &vbo);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glDisable(GL_DITHER);                          /* (on by default: noise in RGB565) */
  if (glGetError() != GL_NO_ERROR) {
    snprintf(err, sizeof err, "GL setup failed");
    return -1;
  }
  return 0;
}

static void drop_targets(void) {
  for (int i = 0; i < GPU_MAX_BUF; i++) {
    target_t *t = &tg[i];
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (t->rb) glDeleteRenderbuffers(1, &t->rb);
    if (t->img) eglDestroyImage(dpy, t->img);
    if (t->map) munmap(t->map, t->map_len);
    if (t->fd >= 0) close(t->fd);
    memset(t, 0, sizeof *t);
    t->fd = -1;
  }
  tgt.nbuf = 0;
}

/* the display's buffers as render targets; when the GPU can't draw into them (an unusual layout),
 * frames go through glReadPixels into the buffers mapped instead */
static int set_target(const gpu_msg_t *m, const int *fds, int nfd) {
  drop_targets();
  if (m->nbuf == 0 && nfd == 0) return 0;      /* let go of them: the display makes new ones */
  int bpp = m->fourcc == DRM_FORMAT_XRGB8888 ? 4 : 2;
  if (m->nbuf < 1 || m->nbuf > GPU_MAX_BUF || nfd != m->nbuf || m->w < 1 || m->h < 1 || m->rw < 1 || m->rh < 1 ||
      m->rw > 4096 || m->rh > 4096 || (size_t)m->rw * (size_t)m->rh * 2 > GPU_SHM_READ_MAX ||
      m->pad < 0 || m->pad > 1 || m->pitch < m->w * bpp ||
      (m->rows == 0 && (m->w != m->rw || m->h != m->rh)) || m->rows < 0 || m->rows > 2 ||
      (m->rows && (m->w != m->rw || m->h - 2 * m->pad < 1 || (m->h - 2 * m->pad) * m->rows != m->rh)) ||
      (m->fourcc != DRM_FORMAT_RGB565 && m->fourcc != DRM_FORMAT_XRGB8888)) {
    for (int i = 0; i < nfd; i++) close(fds[i]);
    snprintf(err, sizeof err, "bad target");
    return -1;
  }
  direct = true;
  for (int i = 0; i < m->nbuf; i++) {
    target_t *t = &tg[i];
    t->fd = fds[i];
    EGLAttrib ia[] = { EGL_WIDTH, m->w, EGL_HEIGHT, m->h, EGL_LINUX_DRM_FOURCC_EXT, m->fourcc,
                       EGL_DMA_BUF_PLANE0_FD_EXT, t->fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                       EGL_DMA_BUF_PLANE0_PITCH_EXT, m->pitch, EGL_NONE };
    t->img = eglCreateImage(dpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, ia);
    if (t->img) {
      glGenRenderbuffers(1, &t->rb);
      glBindRenderbuffer(GL_RENDERBUFFER, t->rb);
      glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, t->img);
      glGenFramebuffers(1, &t->fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t->rb);
    }
    if (!t->img || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) direct = false;
  }
  for (int i = 0; i < m->nbuf && !direct; i++) {
    target_t *t = &tg[i];
    t->map_len = (size_t)m->pitch * (size_t)m->h;
    t->map = mmap(NULL, t->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, t->fd, 0);
    if (t->map == MAP_FAILED) {
      t->map = NULL;
      snprintf(err, sizeof err, "the display's buffers can't be drawn into or mapped (%s)", strerror(errno));
      drop_targets();
      return -1;
    }
  }
  /* the frame is drawn on a texture of its own, which keeps it for the next (as lcd.c's fb does), then
   * goes out (as it is, or through the filter) into the buffer */
  if (tex_fbo(&scene_tex, &scene_fbo, m->rw, m->rh, true)) return -1;
  if (!direct && tex_fbo(&out_tex, &out_fbo, m->w, m->h, false)) return -1;
  size_t need = (size_t)(m->rw > m->w ? m->rw : m->w) * (size_t)(m->rh > m->h ? m->rh : m->h) * 4;
  if (need > rgba_tmp_n) {
    free(rgba_tmp);
    rgba_tmp = malloc(need);
    rgba_tmp_n = rgba_tmp ? need : 0;
    if (!rgba_tmp) {
      snprintf(err, sizeof err, "out of memory");
      return -1;
    }
  }
  tgt = *m;
  if (glGetError() != GL_NO_ERROR) {
    snprintf(err, sizeof err, "GL error setting up the target");
    return -1;
  }
  return 0;
}

static uint16_t rgb565(const uint8_t *p) {
  return (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
}

/* the bound framebuffer's w x h pixels into the display buffer t (its layout), or as RGB565 rows */
static void read_into(int w, int h, uint8_t *dst, int pitch, uint32_t fourcc) {
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba_tmp);
  for (int y = 0; y < h; y++) {
    const uint8_t *s = rgba_tmp + (size_t)y * w * 4;
    uint8_t *d = dst + (size_t)y * pitch;
    if (fourcc == DRM_FORMAT_XRGB8888)
      for (int x = 0; x < w; x++) ((uint32_t *)d)[x] = (uint32_t)s[4 * x] << 16 | (uint32_t)s[4 * x + 1] << 8 | s[4 * x + 2];
    else
      for (int x = 0; x < w; x++) ((uint16_t *)d)[x] = rgb565(s + 4 * x);
  }
}

static int frame(const gpu_msg_t *m, float *gpu_ms) {
  double t0 = now_ms();
  if (!tgt.nbuf || m->buf < 0 || m->buf >= tgt.nbuf) {
    snprintf(err, sizeof err, "no target");
    return -1;
  }
  if (m->ncmd > GPU_SHM_CMD_MAX / sizeof(rcmd_t) || m->atlas_h < 0 || m->atlas_h > GPU_ATLAS_MAXH) {
    snprintf(err, sizeof err, "bad frame");
    return -1;
  }
  target_t *t = &tg[m->buf];
  /* a frame that starts with a clear (the game's do) clears with glClear: the tiles then start from that
   * color instead of loading the last frame from memory */
  const rcmd_t *calls = (const rcmd_t *)(shm + GPU_SHM_CMD);
  int ncalls = (int)m->ncmd;
  bool cleared = ncalls > 0 && calls[0].op == RC_CLEAR;
  if (cleared) {
    calls++;
    ncalls--;
  }
  double tb = now_ms();
  if (gmesh_build(&mesh, calls, ncalls, tgt.rw, tgt.rh)) {
    snprintf(err, sizeof err, "out of memory");
    return -1;
  }
  /* the frame's glyph masks */
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, atlas_tex);
  if (m->atlas_h > 0)
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GPU_ATLAS_W, m->atlas_h, GL_ALPHA, GL_UNSIGNED_BYTE, shm + GPU_SHM_ATLAS);
  glActiveTexture(GL_TEXTURE0);
  /* the calls */
  glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
  glViewport(0, 0, tgt.rw, tgt.rh);
  if (cleared) {
    uint16_t c = calls[-1].c;
    glClearColor((c >> 11) / 31.0f, ((c >> 5) & 63) / 63.0f, (c & 31) / 31.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glUseProgram(prog_scene);
  glUniform2f(u_scene_s, 2.0f / tgt.rw, 2.0f / tgt.rh);
  glUniform2f(u_scene_asz, GPU_ATLAS_W, GPU_ATLAS_MAXH);
  glUniform1i(u_scene_atlas, 1);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)mesh.nv * sizeof(gvert_t)), mesh.v, GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(gvert_t), (void *)offsetof(gvert_t, x));
  glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(gvert_t), (void *)offsetof(gvert_t, u));
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, 1, sizeof(gvert_t), (void *)offsetof(gvert_t, r));
  glEnable(GL_BLEND);
  for (int i = 0; i < mesh.nb; i++) {
    if (mesh.b[i].mode == GB_INVERT) glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ZERO);
    else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, mesh.b[i].first, mesh.b[i].count);
  }
  glDisable(GL_BLEND);
  glDisableVertexAttribArray(1);
  glDisableVertexAttribArray(2);
  double ts = 0, ti = now_ms();
  if (prof) {
    glFinish();
    ts = now_ms();
  }
  /* out into the buffer: as it is, or interlaced through the filter (and a doubled frame's line pairs);
   * the buffer cleared first, so its tiles aren't loaded from memory only to be painted over */
  {
    glBindFramebuffer(GL_FRAMEBUFFER, direct ? t->fbo : out_fbo);
    glViewport(0, 0, tgt.w, tgt.h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog_field);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    int strength = tgt.rows ? tgt.strength : 0;
    float e = strength > 0 ? 1.0f : 0.0f, mm = strength >= 2 ? 2.0f : strength == 1 ? 6.0f : 1.0f;
    glUniform1i(u_field_t, 0);
    glUniform2f(u_field_tsz, (float)tgt.rw, (float)tgt.rh);
    glUniform1f(u_field_rows, (float)(tgt.rows ? tgt.rows : 1));
    glUniform1f(u_field_pad, (float)tgt.pad);
    glUniform1f(u_field_e, e);
    glUniform1f(u_field_m, mm);
    glUniform1f(u_field_div, (2 * e + mm) * (tgt.rows ? tgt.rows : 1));
    glUniform1f(u_field_hl, (float)(tgt.h - 2 * tgt.pad));
    glUniform1f(u_field_tt, 2 * e / (2 * e + mm));
    static const float full[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, full);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  if (!direct) read_into(tgt.w, tgt.h, t->map, tgt.pitch, tgt.fourcc);
  glFinish();
  if (m->readback) {
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
    read_into(tgt.rw, tgt.rh, shm + GPU_SHM_READ, tgt.rw * 2, DRM_FORMAT_RGB565);
  }
  GLenum ge = glGetError();
  if (ge != GL_NO_ERROR) {
    snprintf(err, sizeof err, "GL error 0x%x", ge);
    return -1;
  }
  *gpu_ms = (float)(now_ms() - t0);
  if (prof) {
    double te = now_ms();
    prof_t[1] += ti - tb;
    prof_t[2] += ts - ti;
    prof_t[3] += te - ts;
    prof_t[4] += te - t0;
    prof_verts += mesh.nv;
    prof_calls += ncalls;
    if (++prof_n == 120) {
      fprintf(stderr, "sticktime-gpu: a frame: %.2f ms = build + issue %.2f, scene %.2f, out %.2f (%ld calls, %ld vertices)\n",
              prof_t[4] / prof_n, prof_t[1] / prof_n, prof_t[2] / prof_n, prof_t[3] / prof_n, prof_calls / prof_n,
              prof_verts / prof_n);
      memset(prof_t, 0, sizeof prof_t);
      prof_n = prof_verts = prof_calls = 0;
    }
  }
  return 0;
}

/* ---------------------------------------------------------------- the socket */
static int send_msg(int sock, uint32_t type, uint32_t frame_no, float ms, const char *text) {
  gpu_msg_t m;
  memset(&m, 0, sizeof m);
  m.type = type;
  m.proto = GPU_PROTO;
  m.frame = frame_no;
  m.gpu_ms = ms;
  if (text) snprintf(m.text, sizeof m.text, "%s", text);
  return send(sock, &m, sizeof m, MSG_NOSIGNAL) == (ssize_t)sizeof m ? 0 : -1;
}

static int recv_msg(int sock, gpu_msg_t *m, int *fds, int *nfd) {
  char cbuf[CMSG_SPACE(sizeof(int) * GPU_MAX_BUF)];
  struct iovec io = { .iov_base = m, .iov_len = sizeof *m };
  struct msghdr h = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
  ssize_t r;
  do r = recvmsg(sock, &h, MSG_CMSG_CLOEXEC);
  while (r < 0 && errno == EINTR);
  *nfd = 0;
  if (r < 0) return -1;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      for (int i = 0; i < n; i++) {
        int fd;
        memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
        if (*nfd < GPU_MAX_BUF) fds[(*nfd)++] = fd;
        else close(fd);
      }
    }
  return r == (ssize_t)sizeof *m ? 0 : -1;
}

/* ---------------------------------------------------------------- --test: a buffer of its own */
static int self_test(void) {
  char renderer[128] = "";
  double t0 = now_ms();
  if (gl_init(renderer, sizeof renderer)) {
    printf("GPU: %s\n", err);
    return 1;
  }
  printf("GPU: %s (EGL and GL set up in %.0f ms)\n", renderer, now_ms() - t0);
  int kfd = -1;
  for (int i = 0; i < 4 && kfd < 0; i++) {
    char p[32];
    snprintf(p, sizeof p, "/dev/dri/card%d", i);
    int fd = open(p, O_RDWR | O_CLOEXEC);
    struct drm_mode_card_res r = { 0 };
    if (fd >= 0 && !ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) && r.count_crtcs > 0) kfd = fd;
    else if (fd >= 0) close(fd);
  }
  if (kfd < 0) {
    printf("no display card\n");
    return 1;
  }
  shm = calloc(1, GPU_SHM_SIZE);
  static const int sizes[][4] = { { 837, 470, 1, 0 }, { 1674, 940, 2, 1 }, { 754, 564, 0, 0 } };
  int fails = 0;
  for (int s = 0; s < 3; s++) {
    int rw = sizes[s][0], rh = sizes[s][1], rows = sizes[s][2], pad = sizes[s][3];
    int w = rw, h = rows ? rh / rows + 2 * pad : rh;
    struct drm_mode_create_dumb cd = { .width = (uint32_t)((w + 15) & ~15), .height = (uint32_t)h, .bpp = 16 };
    if (ioctl(kfd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { printf("no dumb buffer: %s\n", strerror(errno)); return 1; }
    struct drm_prime_handle ph = { .handle = cd.handle, .flags = DRM_CLOEXEC | DRM_RDWR };
    if (ioctl(kfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph)) { printf("no dma-buf: %s\n", strerror(errno)); return 1; }
    gpu_msg_t m;
    memset(&m, 0, sizeof m);
    m.nbuf = 1; m.w = w; m.h = h; m.pitch = (int)cd.pitch; m.fourcc = DRM_FORMAT_RGB565;
    m.rw = rw; m.rh = rh; m.rows = rows; m.pad = pad; m.strength = 2;
    if (set_target(&m, &ph.fd, 1)) { printf("%dx%d: %s\n", rw, rh, err); fails++; continue; }
    /* a white bar on the top two render rows over a dark blue screen, and many triangles */
    rcmd_t *c = (rcmd_t *)(shm + GPU_SHM_CMD);
    int n = 0;
    memset(c, 0, sizeof(rcmd_t) * 4000);
    c[n].op = RC_CLEAR; c[n++].c = 0x0010;
    srand(1);
    for (int i = 0; i < 2500; i++, n++) {
      c[n].op = RC_TRI; c[n].c = (uint16_t)rand(); c[n].opa = (uint8_t)(i % 3 ? 0 : 6);
      int x = rand() % rw, y = rand() % rh;
      c[n].u.p.x0 = x; c[n].u.p.y0 = y; c[n].u.p.x1 = x + rand() % 60 - 30; c[n].u.p.y1 = y + rand() % 60 - 30;
      c[n].u.p.x2 = x + rand() % 60 - 30; c[n].u.p.y2 = y + rand() % 60 - 30;
    }
    c[n].op = RC_RECT; c[n].c = 0xFFFF; c[n].u.r.x = 0; c[n].u.r.y = 0; c[n].u.r.w = rw; c[n++].u.r.h = rows == 2 ? 4 : 2;
    m.type = GM_FRAME; m.buf = 0; m.ncmd = (uint32_t)n; m.atlas_h = 0;
    float best = 1e9f, ms;
    for (int rep = 0; rep < 20; rep++) {
      if (frame(&m, &ms)) { printf("%dx%d: %s\n", rw, rh, err); fails++; break; }
      if (ms < best) best = ms;
    }
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    ioctl(kfd, DRM_IOCTL_MODE_MAP_DUMB, &md);
    uint16_t *px = mmap(NULL, cd.size, PROT_READ, MAP_SHARED, kfd, (off_t)md.offset);
    int top = rows ? pad + 0 : 0;                /* the output line of render row 0 */
    uint16_t a = px[(size_t)top * (cd.pitch / 2) + 5], b = px[(size_t)(h - 1 - top) * (cd.pitch / 2) + 5];
    bool up = a > 0x8000, down = b > 0x8000;
    printf("%dx%d %s%s: %.2f ms a frame (%d calls); %s\n", rw, rh, rows ? "interlaced out" : "as it is",
           direct ? "" : " (through glReadPixels)", best, n,
           up && !down ? "the top is at the top" : down && !up ? "UPSIDE DOWN" : "the bar is missing");
    fails += !(up && !down);
    munmap(px, cd.size);
    drop_targets();
  }
  return fails ? 1 : 0;
}

int main(int argc, char **argv) {
  prof = getenv("STP_GPU_PROF") != NULL;
  if (argc > 1 && !strcmp(argv[1], "--test")) return self_test();
  int sock = GPU_FD_SOCK;
  shm = mmap(NULL, GPU_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, GPU_FD_SHM, 0);
  if (shm == MAP_FAILED) {
    send_msg(sock, GM_ERROR, 0, 0, "no shared memory");
    return 1;
  }
  for (int i = 0; i < GPU_MAX_BUF; i++) tg[i].fd = -1;
  gpu_msg_t m;
  int fds[GPU_MAX_BUF], nfd;
  if (recv_msg(sock, &m, fds, &nfd) || m.type != GM_HELLO || m.proto != GPU_PROTO) return 1;
  if (m.cmd_size != sizeof(rcmd_t) || m.msg_size != sizeof(gpu_msg_t)) {
    send_msg(sock, GM_ERROR, 0, 0, "sticktime-gpu doesn't belong to this sticktime-pi (another version)");
    return 1;
  }
  char renderer[128] = "";
  if (gl_init(renderer, sizeof renderer)) {
    send_msg(sock, GM_ERROR, 0, 0, err);
    return 1;
  }
  send_msg(sock, GM_READY, 0, 0, renderer);
  for (;;) {
    if (recv_msg(sock, &m, fds, &nfd)) break;          /* the host is gone */
    if (m.type == GM_QUIT) break;
    if (m.type == GM_TARGET) {
      if (set_target(&m, fds, nfd)) send_msg(sock, GM_ERROR, 0, 0, err);
      else send_msg(sock, GM_DONE, 0, 0, !m.nbuf ? "dropped" : direct ? "direct" : "read back");
    } else if (m.type == GM_FRAME) {
      float ms = 0;
      if (frame(&m, &ms)) send_msg(sock, GM_ERROR, m.frame, 0, err);
      else send_msg(sock, GM_DONE, m.frame, ms, NULL);
    }
    for (int i = 0; i < nfd && m.type != GM_TARGET; i++) close(fds[i]);
  }
  drop_targets();
  return 0;
}
#endif
