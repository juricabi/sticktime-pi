/* Single-pin soft serial for VTX control: one GPIO for both directions, like a flight controller's
 * half-duplex soft serial (Betaflight's softserial is the precedent). A vtx_io_t line for vtx.c, whose
 * protocol logic doesn't change.
 *
 * Receive: the pin is an input with edge events on both edges (the kernel's timestamps, CLOCK_MONOTONIC)
 * and a bias: pull-down for SmartAudio (both sides inputs when idle, the line low before a frame: the
 * TBS spec; Betaflight pulls SmartAudio down), pull-up for Tramp (Betaflight opens Tramp as plain half
 * duplex: open-drain with a pull-up, idle high). The UART bytes are decoded from the edge times.
 * Send: the same pin drives the frame. Every edge is driven at its own absolute time, counted from the
 * frame's start so errors never add up: clock_nanosleep(TIMER_ABSTIME) to ~60 us before it, then a spin
 * on CLOCK_MONOTONIC. For the frame the thread runs SCHED_FIFO above the drawing threads that share its
 * core (vtx_soft_set_rt_prio). On a Pi (BCM283x/2711) the pin's function and level are written straight
 * into the GPIO registers through /dev/gpiomem, as pigpio does: the character device keeps the line as
 * an input with edge detection the whole time, so the kernel's edge interrupt (a thread, raised above
 * the sender) stays armed and no setup is torn down and built again for every frame (through the ioctls
 * that took up to 7 ms on a busy Pi 4, and the VTX's reply had begun). The sender's own edges come back
 * as events too; they are told apart by their timestamps and counted (a wiring check). Elsewhere, and
 * on another chip, the line is switched between input and output with the ioctls. An edge later than
 * half a bit makes the frame go again (a UART reads a bit in its middle). What was driven is handed
 * back as the line's "echo", so vtx.c's echo check sees it.
 *
 * The decoder and the bit schedule are pure functions, tested on every OS; the GPIO part is Linux only. */
#ifndef VTX_SOFT_H
#define VTX_SOFT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vtx.h"

typedef struct { int64_t t_ns; uint8_t level; } vtx_edge_t;

/* ---------------------------------------------------------------- the bit schedule */
/* The edges that send p[0..n) as UART frames (start bit, 8 data bits LSB first, stop_bits stop bits, no
 * gaps) from time 0, the line at level `from` before. Times are rounded from the exact bit times (no
 * drift). Returns the number of edges (at most max); *end_ns = the end of the last stop bit. */
int vtx_soft_schedule(const uint8_t *p, size_t n, int baud, int stop_bits, int from, vtx_edge_t *out, int max, int64_t *end_ns);

/* ---------------------------------------------------------------- the decoder */
#define VTX_SOFT_EDGES 24

typedef struct {
  int64_t bit_ns;                  /* nominal bit time */
  int stop_bits;
  int level;                       /* the line now */
  bool in_byte;
  int64_t start_ns;                /* the falling edge of the start bit */
  vtx_edge_t e[VTX_SOFT_EDGES];    /* edges of the byte since its start */
  int ne;
  uint32_t bytes, framing, glitches, breaks;
} vtx_soft_rx_t;

void vtx_soft_rx_init(vtx_soft_rx_t *r, int baud, int stop_bits, int level);
/* The line went to `level` at t_ns. Finished bytes go to out (returns how many, at most 2). A start bit
 * shorter than half a bit is a glitch; the bit time is re-measured from the byte's clean edges (the spec's
 * +-5 % VTX clock); each bit is sampled in its middle; a low stop bit is a framing error (the byte is
 * kept: the CRC decides), a whole low "byte" is the line resting low (dropped, counted as a break). */
int  vtx_soft_rx_edge(vtx_soft_rx_t *r, int64_t t_ns, int level, uint8_t *out, int max);
/* No edge until now_ns: finishes a byte whose stop bit has passed. */
int  vtx_soft_rx_idle(vtx_soft_rx_t *r, int64_t now_ns, uint8_t *out, int max);
/* When the byte in progress can be finished (0 = none). */
int64_t vtx_soft_rx_due(const vtx_soft_rx_t *r);

/* ---------------------------------------------------------------- ports and the line */
/* the single pins vtx_port offers (BCM GPIO number and header pin), and the spec forms */
bool vtx_soft_is_spec(const char *spec);         /* "gpioN", "gpio:/dev/gpiochipX:OFFSET" */
int  vtx_soft_header_pin(int gpio);              /* 4 -> 7, ...; 0 if not one of ours */
/* the line; NULL + err when the spec isn't a GPIO one (or out of memory). Not Linux: a line whose open()
 * says GPIO pins work on the Pi only. */
vtx_io_t *vtx_soft_io_new(const char *spec, char *err, size_t errlen);
/* The SCHED_FIFO priority the line's thread takes for each frame (default 40: a line test run by hand).
 * The VTX thread sets it to the frame loop's priority minus one: above the drawing threads (minus two)
 * that share its core, which otherwise preempt a frame for milliseconds (seen on a Pi 4: edges 1.3 ms
 * late on average, every frame sent again, a Tramp VTX answered 4 frames of 1700). */
void vtx_soft_set_rt_prio(int prio);

#endif
