/* Process setup for low latency and the console, Linux only (no-ops elsewhere). */
#ifndef SYS_H
#define SYS_H

#include <stdbool.h>
#include <stddef.h>

/* SCHED_FIFO at prio for the calling thread (0 = leave), mlockall, pin to cpu (-1 = leave).
 * Failures are reported in msg (not fatal). */
void sys_realtime(int prio, int cpu, char *msg, size_t msglen);
/* Put the console (a VT, default /dev/tty1) into graphics mode before video_open():
 * brings that VT to the front, hides the cursor, stops blanking, KDSKBMODE K_OFF (no typed
 * characters, no Alt+Fn VT switches, no Ctrl-Alt-Del; evdev readers still get every key) and
 * KDSETMODE KD_GRAPHICS (fbcon stops drawing). Failures are not fatal (no VT: nothing to do).
 * sys_console_restore() undoes it (KD_TEXT first: fbcon takes the display back while we are still
 * DRM master; then the saved keyboard mode). It is async-signal-safe and is also run at exit and
 * on fatal signals (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGQUIT), which are then re-raised.
 * (SIGKILL can't be caught: the service's TTYReset=yes restores the VT then.) */
void sys_console_graphics(const char *tty);
void sys_console_restore(void);
/* After sys_console_graphics(): put the VT back into KD_GRAPHICS + K_OFF if someone switched it to
 * text mode meanwhile (video calls it when it takes the display back). Not signal-safe. */
void sys_console_refresh(void);
/* true while sys_console_graphics() is in effect */
bool sys_console_is_graphics(void);
/* true when our VT is the one in front (or there is no VT handling): video only takes the
 * display back from someone else when this is true */
bool sys_console_foreground(void);
/* For "--restore-vt" / ExecStopPost: put a VT back into text mode with a normal keyboard and a
 * visible cursor, from any process. 0 ok, -1 the tty can't be opened or changed. */
int  sys_console_reset(const char *tty);
/* Install SIGINT/SIGTERM/SIGHUP handlers that set the returned flag (frame loop polls it).
 * No SA_RESTART, so blocking calls (ppoll) return EINTR. A second signal while the flag is
 * already set restores the console and terminates at once (a hung loop can still be stopped).
 * SIGPIPE is ignored (sockets). */
volatile int *sys_quit_flag(void);
/* CPU temperature in degrees C, NaN when unknown */
double sys_cpu_temp(void);
/* first IPv4 address of a non-loopback interface ("" when none); iface gets its name */
void sys_ip_address(char *ip, size_t iplen, char *iface, size_t ifacelen);

#endif
