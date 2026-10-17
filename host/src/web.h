/* The web server: HTTP/1.1 on port 80 (static files + JSON API), one worker thread per
 * connection (capped), never touching the frame loop's data except through app_t.
 *
 * Routes (see docs/WEB_API.md):
 *   GET  /                      web/index.html (the settings app); other files from web/
 *   GET  /editor/               the StickTime emulator page (web/editor/simulator.html) with
 *                               <script src="/pi-sd.js"> injected before </body>
 *   GET  /api/status            JSON status (app_status_t + versions + network)
 *   GET  /api/settings          {"values":{...},"keys":[...],"warnings":"..."}
 *   POST /api/settings          body: "key = value" lines; validates, saves, applies
 *   GET  /api/sticks            JSON of all sources' channels (poll ~20 Hz)
 *   GET  /api/sticks/stream     text/event-stream of the same, 30 Hz
 *   GET  /api/sd/<path>         a file, or a directory listing (JSON) when <path> is a folder
 *   PUT  /api/sd/<path>         write a file (atomic; parent folders created)
 *   DELETE /api/sd/<path>       delete a file or a folder (?recursive=1)
 *   POST /api/sd/<path>?mkdir=1 create a folder
 *   POST /api/game/reload       reload the game
 *   POST /api/key               body "enter|exit|next|prev|inc|dec|nextpage|prevpage|menu"
 *   GET  /api/screenshot.png    the current frame
 *   GET  /api/log               recent log lines
 *   GET  /api/wifi/scan         networks (runs the helper, cached a few seconds)
 *   POST /api/wifi              body "ssid = ...\npassword = ...\ncountry = .." -> settings + apply
 *   GET  /api/update            update status (helper's JSON state file)
 *   POST /api/update/check      start a check;  POST /api/update/apply  body "game" | "app"
 *   GET/POST /api/vtx            VTX control state; set band/channel/freq/power/pit (web_vtx.c)
 *   GET/PUT/DELETE /api/vtx/table  the VTX table (Betaflight Configurator JSON)
 *   POST /api/reboot, /api/shutdown
 * Mutating requests (POST/PUT/DELETE) must carry the header "X-StickTime: 1" (blocks
 * cross-site requests without a CORS preflight, which this server never grants) and a Host
 * header that is an IP address, "localhost", or the hostname / hostname.local (DNS rebinding). */
#ifndef WEB_H
#define WEB_H

#include <stdint.h>
#include <stdbool.h>
#include "app.h"

typedef struct {
  const char *bind_addr;       /* NULL = any */
  uint16_t port;               /* 80 */
  int max_clients;             /* 16 */
  const char *hostname;        /* accepted Host names besides IPs and localhost */
  size_t max_upload;           /* bytes, PUT /api/sd (default 4 MB) */
} web_config_t;

/* start the server thread; 0 ok, -1 if the port can't be bound (logged) */
int  web_start(app_t *app, const web_config_t *cfg);
void web_stop(void);

/* docs/WEB_API.md is the authoritative route list. max_upload caps PUT /api/songs (default 64 MiB);
 * /api/sd bodies are capped at 64 KB for a track slot and 2 MiB otherwise, as WEB_API.md says. */

/* True once after the web API changed a track slot (SCRIPTS/TOOLS/StickTimeTracks/c1..c8.txt) or a
 * game file (StickTime.lua, the BW/Lite editions, a directory holding them): the frame loop then
 * reloads the game, debounced, since "Copy all" writes up to 8 files. Lock-free (an atomic
 * exchange), so it is cheap to call every frame. */
bool web_take_sd_dirty(void);

/* ---- helpers shared with tests ---- */
/* Normalize an /api/sd path: percent-decoded, '/'-separated, no "." or ".." segments, no
 * backslashes, NUL, control characters or drive letters. Returns 0 and the cleaned relative
 * path (no leading '/', may be "" for the root) or -1 if unsafe. */
int web_clean_path(const char *in, char *out, size_t outlen);
/* percent-decode in place ('+' stays '+'); returns -1 on bad escapes or %00 */
int web_url_decode(char *s);
/* JSON string escaping into out (always NUL-terminated); returns the needed length */
size_t web_json_escape(const char *s, char *out, size_t outlen);

#endif
