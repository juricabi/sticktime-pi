# StickTime Pi web API

The host (`host/src/web.c`, `web_api.c`) serves HTTP/1.1 on port 80: the settings app (`web/`), the
StickTime emulator page at `/editor/` and this JSON API. Clients: the settings app (`web/app.js`),
`web/pi-sd.js` (the File System Access shim injected into the emulator page) and the tests.

## Conventions

- JSON responses: `application/json; charset=utf-8`, `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`.
  No CORS headers anywhere. Times are milliseconds since the epoch unless named `_us`/`_s`.
- Errors: status code + `{"error":"<Name>","message":"<plain English>"}`. Names are DOMException names
  where the editor shim needs them (`NotFoundError`, `TypeMismatchError`, `NoModificationAllowedError`,
  `QuotaExceededError`, `InvalidModificationError`, `NotReadableError`, `TypeError`) and otherwise
  `BadRequest`, `Forbidden`, `NotFound`, `MethodNotAllowed`, `PayloadTooLarge`, `Busy`, `Internal`.
- **Mutating requests** (POST, PUT, DELETE) must send `X-StickTime: 1`, else 403 `Forbidden`. A cross-site page
  can't add that header without a CORS preflight, which this server never grants. If an `Origin` header is
  present it must match the `Host` header.
- **Host check** (DNS rebinding): `Host` must be an IP literal (v4, or v6 in brackets), `localhost`, the
  configured hostname or `<hostname>.local` (any port, case-insensitive); else 421 `Forbidden`.
- Request limits: request line + headers 16 KB; bodies need `Content-Length` (chunked → 411); bodies over
  the route's limit → 413. Keep-alive is supported; idle connections close after 15 s; at most 16 clients
  (more → 503 `Busy`).
- Text bodies "in settings format" are `key = value` lines exactly as in `sticktime.txt` (see settings.h);
  `application/x-www-form-urlencoded` is accepted too for those routes.

## Pages

| Route | Serves |
|---|---|
| `GET /` | `web/index.html` (the settings app). Other files from `web/` by path (no directory listings). |
| `GET /editor` | 301 → `/editor/` |
| `GET /editor/`, `/editor/index.html` | `editor/index.html`: StickTime's `simulator.html` of the installed game version, patched (pi-sd.js injected before `</head>`, Google Fonts links removed) |
| `GET /editor/pi-sd.js` | `web/pi-sd.js` |
| `GET /editor/tracks/index.json` | the track gallery (from the StickTime site), 404 when missing (the page then uses its built-in gallery) |
| `GET /editor/sdcard/manifest.json`, `/editor/sdcard/SCRIPTS/TOOLS/...` | the staged StickTime release for the page's **Install** (see Updates); 404 when nothing is staged |
| `GET /favicon.ico` | 204 |

Static files get `ETag` + `Cache-Control: no-cache`; `If-None-Match` → 304. Types by extension: html, js, css,
json, png, svg, ico, txt, woff2, wav, mp3, ogg.

## Status

`GET /api/status` →
```json
{"version":"0.1.0","game_version":"1.7.2","uptime_ms":123456,
 "fps":60.0,"frame_ms":{"avg":16.7,"max":17.1},"run_ms":{"avg":3.1,"p95":4.0},"present_ms":0.6,
 "latency_ms":9.8,"frames":7400,"missed":3,
 "game":{"state":1,"state_name":"menu","loaded":true,"error":"","lua_kb":812},
 "video":{"desc":"Composite-1 720x480i 59.94 Hz, NTSC 4:3, render 640x480","render_w":640,"render_h":480},
 "input":{"active":0,"kind":"wifi","live":true,
          "sticks":{"ail":0,"ele":0,"thr":-1024,"rud":0,"pause":-1024},
          "sources":[{"kind":"wifi","name":"ELRS 10.0.0.1","live":true,"rate_hz":250.0,"nch":16}]},
 "wifi_joystick":{"module":"ELRS 10.0.0.1","state":"streaming","rate_hz":250.0,"dropped":0,"gaps":0,"gap_max_ms":0},
 "audio":{"device":"jack","ok":true,"music":"Song 3 (generated)"},
 "network":{"ip":"192.168.1.23","iface":"wlan0","ssid":"Home","mode":"client","hostname":"sticktime"},
 "system":{"cpu_temp":51.2,"throttled":"0x0"},
 "vtx":{"state":"ready","text":"SmartAudio 2.1 · R1 5658 MHz · 25 mW"}}
```
State names: unknown, menu, setup, count, fly, crashed, ready, paused, done. `vtx` is one line for the status
page (states as in `GET /api/vtx`; `text` is `off` when VTX control is off). `system.throttled`: the firmware's
throttling flags in hex, as `vcgencmd get_throttled` prints them (bit 0 under-voltage now, 1 frequency capped,
2 throttled, 3 soft temperature limit; bits 16-19 the same since boot), read from the firmware's mailbox
(`/dev/vcio`) where the kernel has no `get_throttled` file (6.12); `""` when unknown (not a Pi).

`GET /api/sticks` → `{"sources":[{"kind":"wifi","name":"...","live":true,"rate_hz":250,"nch":16,"ch":[16 ints]}],"active":0,"sticks":{...}}`
(poll it about 20 times a second). `GET /api/sticks/stream` → `text/event-stream`, one `data: <same JSON>` event
every 33 ms until the client goes away (counts as a client).

`GET /api/log` → `text/plain`, the recent log lines. `GET /api/screenshot.png` → the current frame (render size), PNG.

## Settings

`GET /api/settings` →
```json
{"values":{"video":"av","tv":"ntsc", ...every known key, secrets as "" with "<key>_set":true|false...},
 "keys":[{"key":"video","kind":"enum","default":"av","choices":["av","hdmi"],"live":false,"reboot":true,"help":"..."}, ...],
 "warnings":"line 12: no '='\n"}
```
Secrets (`wifi_password`, `hotspot_password`, `password`, `github_token`) are never sent back; `values` has `""`
and `"<key>_set": true` when the file has a value.

`POST /api/settings` (settings-format body, only the keys to change; an empty value clears a key, a secret
sent as empty is left unchanged unless the body has `<key>_clear = 1`) →
`{"ok":true,"saved":["tv"],"reboot":true,"restart":false}` or 400 `{"error":"BadRequest","message":"...","fields":{"tv":"must be one of ..."}}`
(nothing is saved when any field is invalid). Unknown keys are kept in the file. Live keys apply at once; keys
that need the host restarted (`video` → `aspect`, `av_mode`, `av_detail`, `av_flicker`, `hdmi_mode`, `margin`, `fonts`) restart the
service; `reboot` keys need `POST /api/reboot` (the app offers it).

## The game's SD card

`/api/sd/<path>`: paths relative to the SD folder (`/boot/firmware/sticktime/sd`), `/`-separated components,
each percent-encoded. A component is refused (400 `TypeError`) when empty (except a trailing slash), `.`/`..`,
containing NUL, `\`, control characters or `" * : < > ? |`, ending in `.` or space, over 255 bytes, or not UTF-8;
the whole path over 1024 bytes. Components resolve case-insensitively (FAT). Symlinks are never followed.

| Request | Does | Response |
|---|---|---|
| `GET /api/sd/<p>?stat` | stat | `{"name":"c1.txt","kind":"file","size":312,"mtime":1760000000000}` (root: `name ""`, kind directory) |
| `GET /api/sd/<dir>?list` | list | `{"entries":[{"name":..,"kind":..,"size":..,"mtime":..}]}` sorted by name, no temp files; 409 `TypeMismatchError` for a file |
| `GET /api/sd/<file>` | read | bytes, `application/octet-stream`, `X-Mtime-Ms`; 409 for a directory; 413 `NotReadableError` over 16 MiB |
| `PUT /api/sd/<file>` | atomic write (temp + fsync + rename) | `{"size":n,"mtime":..}`; 404 parent missing; 409 a parent is a file / the target a directory; 403 policy; 413/507 `QuotaExceededError` |
| `POST /api/sd/<p>?create=file` | create if missing, no truncate | stat JSON, 201 made / 200 existed |
| `POST /api/sd/<p>?create=directory` | mkdir if missing | stat JSON, 201 / 200 |
| `DELETE /api/sd/<p>[?recursive=1]` | delete | 204; 409 `InvalidModificationError` not empty |

**Write policy** (`sd_write` setting):
- `game` (default): track slots `SCRIPTS/TOOLS/StickTimeTracks/c[1-8].txt`; the game's files as the emulator's
  Install writes them: `SCRIPTS/TOOLS/StickTime.lua`, `StickTimeBW.lua`, `StickTimeLite.lua`,
  `StickTimeBW/<name>`, `StickTimeLite/<name>` (`<name>` = `[A-Za-z0-9_-]+\.(lua|luac|txt|new)`, never `data.txt`),
  `StickTimeTracks/README.txt`, `StickTimeGhosts/README.txt`; directories on those paths. Never saves (`*.dat`),
  ghosts or anything else.
- `tracks`: only the track slots (and their directories). `all`: anything under `sd/`. `off`: nothing.
- Refusals: 403 `NoModificationAllowedError` with a message saying what is allowed.
- Bodies: 64 KB for a track slot, 2 MiB otherwise.
- After a change to a track slot or a game file, the game reloads (at once in the menu or settings, else the next
  time it returns to the menu; 300 ms debounce).

## Game and remote

- `POST /api/game/reload` → `{"ok":true}`.
- `POST /api/key` body `enter|exit|next|prev|inc|dec|nextpage|prevpage|menu|up|down` → `{"ok":true}`
  (`up`/`down` use the editing-aware pitch mapping, like the sticks).
- `GET /api/tools` → `{"current":"/SCRIPTS/TOOLS/StickTime.lua","tools":[{"path":"/SCRIPTS/TOOLS/StickTime.lua","name":"StickTime"},...]}`:
  the color tool scripts in `/SCRIPTS/TOOLS/` (the emulator's "Open .lua"); `name` is the script's `TNS|...|TNE`
  tool name (first KB), else its file name. The image has StickTime's three editions: `StickTime.lua` ("StickTime"),
  `StickTimeBW.lua` ("StickTime BW") and `StickTimeLite.lua` ("StickTime Lite"); the page says which edition each is.
  Choose one with `POST /api/settings` `script = <path>`.

## Audio

- Settings keys: `sound`, `volume`, `music`, `music_volume`, `music_source` (`generated|songs`), `audio_out`.
- `GET /api/songs` → `{"songs":[{"name":"track1.mp3","size":123,"mtime":..}],"free_mb":312}`: the player's own
  songs (mp3, ogg, wav) in `/boot/firmware/sticktime/music/` (also editable from a PC). Names: no path
  separators, the SD component rules above, 120 bytes max.
- `PUT /api/songs/<name>` (body = file, ≤ 64 MiB) → `{"size":n}`; `DELETE /api/songs/<name>` → 204.
- `POST /api/music/next` → skip to the next song.

## Network

- `GET /api/wifi/scan` → `{"networks":[{"ssid":"Home","signal":72,"security":"WPA2","band":"2.4","chan":6,"known":true}],"at":<ms>}`
  (the helper runs `nmcli`; results are cached 10 s; 503 `Busy` while the first scan runs, retry).
- `POST /api/wifi` settings-format body `ssid`, `password`, `country`, optional `hidden = 1` → `{"ok":true}`: saved to
  `sticktime.txt` (`wifi_ssid`, `wifi_password`, `wifi_country`) and applied by the helper in the background (the
  connection may drop: the app says so and polls `/api/status`).
- `POST /api/wifi/module-ap` body `elrs|tbs` → joins the ELRS (`ExpressLRS TX` / `expresslrs`, module 10.0.0.1)
  or TBS module's own access point (sets `wifi_module_ip`).

## VTX control

The VTX thread (`host/src/vtx.c`) sets the analog VTX over SmartAudio or Tramp from the settings `vtx`,
`vtx_port`, `vtx_band`, `vtx_channel`, `vtx_freq`, `vtx_power`, `vtx_pit`, `vtx_send_only` and the VTX table
(`/boot/firmware/sticktime/vtxtable.json`, else a built-in one). These routes never wait for the VTX: they read
the thread's last state and tell it to re-read the files.

`GET /api/vtx` →
```json
{"enabled":true,"state":"ready","message":"SmartAudio 2.1: R1 5658 MHz, 25 mW, pit mode off.","error":"",
 "mode":"auto","port":"gpio4","device":"GPIO4, pin 7 (single pin)","single_pin":true,"send_only":false,"needs_reboot":false,
 "protocol":"smartaudio","version":"2.1","line":"4800 baud 8N2, a 0x00 before each frame",
 "line_stats":"40 frames sent; TX edges late by 6 us on average, 31 us at most (last frame 12 us); 0 sent again; RX 300 bytes, 0 framing errors, 0 glitches",
 "baud":4800,
 "found":true,"in_sync":true,"freq":5658,"freq_mode":false,"band":"R","channel":1,
 "power":{"level":1,"label":"25","value":14,"mw":25},"pit":false,"pit_at_boot":false,"locked":false,
 "temp_c":null,"limits":null,
 "settings":{"band":"R","channel":1,"freq":0,"power":1,"pit":false},
 "table":{"source":"default","error":"","description":"StickTime Pi default: SmartAudio 2.1","channels":8,
          "bands":[{"name":"BOSCAM_A","letter":"A","factory":true,"frequencies":[5865,5845,5825,5805,5785,5765,5745,5725]}, ...],
          "powers":[{"value":14,"label":"25"},{"value":20,"label":"100"},{"value":26,"label":"400"}]},
 "stats":{"sent":40,"replies":38,"timeouts":2,"bad_frames":0,"echo_ok":40,"echo_missing":0,"echo_bad":0},"age_ms":180}
```
- `port`: `gpio4`, `gpio17`, `gpio22`, `gpio23`, `gpio27` (one pin, in software; `single_pin` true) or `uart3`,
  `uart4`, `uart5` (two pins on a PL011 UART). `line_stats`: the one-pin line's timing (how late its edges went out,
  frames sent again because an edge was more than a quarter bit late, receive errors); `""` otherwise.
- `state`: `off`, `no_port` (the UART isn't there: its overlay comes at the next boot, `needs_reboot` true; or the
  tty can't be opened; for a pin: no GPIO chip has it, i.e. not a Pi), `searching`, `not_found` (a whole round without an answer; it keeps trying, `message` says
  what to check), `syncing` (sending the settings), `ready` (set like the settings), `send_only` (sent, nothing read
  back), `error` (a setting the VTX or the table can't do, a VTX that didn't take a setting, a bad table file).
- `protocol`: `smartaudio` or `tramp` (`""` until found); `version`: `1.0`, `2.0` or `2.1` for SmartAudio.
- `band`/`channel`: where the VTX is, as the table says (`""`/0 when its frequency isn't in the table). `power.value`
  is what the VTX reports: the PA DAC value (SmartAudio 1.0), a level 0..3 (2.0), dBm (2.1), mW (Tramp);
  `power.level` is the 1-based level in the table (0 = not in it). `temp_c` and `limits` (`min_freq`, `max_freq`,
  `max_power`) come from Tramp. `pit_at_boot`: SmartAudio's pit-mode flag (2.0 turns pit mode on at the next power-up).
- `settings`: what the VTX should be set to (the settings; `band` `""` and `power` 0 mean "leave it", `freq` 0 means
  "by band and channel").
- `table.bands[].factory`: one of the VTX's own bands (SmartAudio sets it by channel number); others are set by
  frequency. `table.source`: `file` or `default`; `table.error`: why `vtxtable.json` can't be used.

`POST /api/vtx` settings-format body with any of `band` (a letter of the table, `""` = leave the frequency),
`channel` (1..channels), `freq` (MHz: 0 or 4900..6200; overrides band/channel), `power` (1..levels, 0 = leave it),
`pit` (`on|off`); the `vtx_*` names work too → `{"ok":true,"saved":["vtx_band","vtx_channel"]}`: checked against
the table in use, saved to `sticktime.txt` like `POST /api/settings` and applied. A `band` without `freq` also
saves `vtx_freq = 0`. 400 `{"error":"BadRequest","fields":{"band":"isn't a band of the VTX table (A, B, E, F, R)"}}`
(nothing saved), 400 for an empty body, 413 over 4 KB.

`GET /api/vtx/table` → the table in use as Betaflight Configurator JSON, with `Content-Disposition: attachment;
filename="vtxtable.json"`:
`{"description":"...","version":"1.0","vtx_table":{"bands_list":[{"name":"BOSCAM_A","letter":"A","is_factory_band":true,"frequencies":[...]}],"powerlevels_list":[{"value":14,"label":"25"}]}}`.

`PUT /api/vtx/table` body = a Betaflight VTX table file (≤ 64 KB, what VTX makers publish) →
`{"ok":true,"description":"...","bands":5,"channels":8,"powers":4}`: checked (at most 8 bands of 1..8 channels,
frequencies 0 or 1000..9999 MHz, one-character letters used once, 1..8 power levels with values 0..65535 and labels
of up to 7 characters, version 1.x), stored as read back (normalized) to `vtxtable.json` and used at once.
400 `BadRequest` with a message saying what is wrong; 413 over 64 KB.

`DELETE /api/vtx/table` → `{"ok":true}`: the built-in table again (no error when there was no file).

With `POST /api/settings`, turning `vtx` on or changing `vtx_port` answers `"reboot": true` when the settings now need a
UART overlay they didn't before (a `uart*` port: it goes into `config.txt` at the next boot); one pin needs none. All VTX
keys apply at once otherwise.

## Updates

- `GET /api/update` → the updater's state file:
  `{"state":"idle|checking|downloading|installing|done|error","message":"...","progress":0.5,
    "game":{"installed":"1.7.2","latest":"1.7.3","staged":"1.7.3","notes":"..."},
    "app":{"installed":"0.1.0","latest":"0.2.0","notes":"..."},"checked_at":<ms>,"error":""}`
- `POST /api/update/check` → 202 `{"ok":true}` (runs `sticktime-update check`; staging the latest game release also
  makes the editor page's Install offer it).
- `POST /api/update/apply` body `game|app` → 202 (runs `sticktime-update apply <what>`; the app update restarts the
  service, so the page polls `/api/status` until it answers again). An app update or rollback also brings that
  version's system scripts (`lib/` → `/usr/lib/sticktime`); when some of them apply only after a reboot, `message`
  ends with "Some system changes apply after a reboot.", and when they couldn't be installed, with "The system
  scripts weren't updated: …" (the app itself is installed either way).
- `POST /api/update/rollback` body `game|app` → 202.

## System

- `POST /api/reboot`, `POST /api/shutdown` → 202, then the Pi goes down after the response is sent.
- `POST /api/restart` → 202, restarts the sticktime service.
- `GET /api/system` → `{"hostname":"sticktime","ssh":false,"password_set":false,"disk":{"free_mb":..,"total_mb":..},"os":"Raspberry Pi OS ...","kernel":"6.12..","model":"Raspberry Pi 4 Model B Rev 1.5"}`.
