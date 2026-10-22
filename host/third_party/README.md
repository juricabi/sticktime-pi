# Third-party code (unmodified)

| Path | What | Version | License |
|---|---|---|---|
| `lua/` | Lua (github.com/lua/lua) | 5.3.6 | MIT (see the end of `lua.h`) |
| `stb_truetype.h` | github.com/nothings/stb | as of 2026-10-16 | MIT or public domain |
| `stb_image_write.h` | github.com/nothings/stb | as of 2026-10-16 | MIT or public domain |
| `minimp3.h`, `minimp3_ex.h` | MP3 decoder, github.com/lieff/minimp3 | commit ea99364f61c14656440e8d77e9c233ccf3124633 | CC0 1.0 (header comment) |
| `stb_vorbis.c` | Ogg Vorbis decoder, github.com/nothings/stb | v1.22, commit 2c980bb59875b0d32144a71867fbdebb2f77cd20 | MIT or public domain (end of file) |
| `dr_flac.h` | FLAC decoder, github.com/mackron/dr_libs | v0.13.4, commit dfe8377631000664666519fdb83da193fd8037f4 | public domain (Unlicense) or MIT-0 (end of file) |
| `dr_wav.h` | WAV decoder, github.com/mackron/dr_libs | v0.14.6, commit dfe8377631000664666519fdb83da193fd8037f4 | public domain (Unlicense) or MIT-0 (end of file) |
| `elrs-wifi/` | the ELRS / Crossfire WiFi joystick protocol core (`wifi_proto`, `wifi_net` and their tests), github.com/juricabi/elrs-crossfire-wifi-joystick-linux `core/` | the commit in `elrs-wifi/VENDOR` | MIT (`elrs-wifi/LICENSE`) |

Build Lua with EdgeTX's number settings: `-DLUA_32BITS -DLUA_FLOORN2I=1 -DLUA_COMPAT_5_2`.
`ltests.c` and `lua.c` are not part of the library.
The audio decoders are compiled once, inside `src/songs.c` (with their warnings silenced there).
minimp3 and stb_vorbis both have a static `get_bits`: `songs.c` renames stb_vorbis's with a macro around its include (the files stay unmodified).
`elrs-wifi/` is copied by `tools/vendor_elrs_wifi.sh` and checked against `elrs-wifi/VENDOR` in CI: change it upstream,
then vendor it again (`elrs-wifi/README.md`).
