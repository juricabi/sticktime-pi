# elrs-wifi: the ELRS / Crossfire WiFi joystick protocol core (vendored)

**Never edit the files here.** Change them upstream, then vendor them again (below). CI runs
`tools/vendor_elrs_wifi.sh --check`, which fails when a file here differs from what `VENDOR` recorded.

| | |
|---|---|
| Source | [github.com/juricabi/elrs-crossfire-wifi-joystick-linux](https://github.com/juricabi/elrs-crossfire-wifi-joystick-linux), directory `core/` (and its `LICENSE`) |
| Commit | `6380bb85c46ee0fe07b4beb36d6328ea59390294` (also in `VENDOR`, with the SHA-256 of every file copied) |
| License | MIT, (c) juricabi: `LICENSE` |

The same author's Linux WiFi joystick app is built on this core, and both follow the Windows app
([ELRS-Crossfire-WiFi-Joystick-Windows](https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Windows),
`JoystickEngine.cs`). StickTime Pi uses it from `host/src/input_wifi.c`.

| File | What |
|---|---|
| `wifi_proto.c/.h` | the protocol engine: pure logic (beacons, activation, source lock, Crossfire placeholders, stats, the request with its restarts, the activation queue) |
| `wifi_net.c/.h` | the activation POST (`joystick_end` + `joystick_begin`), blocking, for the worker thread; socket helpers |
| `tests/` | its unit tests and their `t.h`; `host/build.sh test` builds and runs `tests/test_*.c` with the host's |
| `VENDOR` | written by the script: where the files came from and their SHA-256 (paths from the repo root) |

The script also copies the fake module the tests use, upstream `tests/fake_elrs.py`, to `host/tests/fake_elrs.py`
(listed in `VENDOR` too): the two copies stay the same.

StickTime Pi compiles it with `-DWIFI_USER_AGENT="sticktime-pi"` (`host/build.sh`), so the module sees the
same User-Agent as before the core moved.

## Updating

1. Make the change in the Linux repo's `core/`, run its tests there (`./build.sh native test`, `SAN=1 WERROR=1`
   too, and the Windows compile check), and commit it.
2. Here, with that repo checked out next to this one:
   ```sh
   tools/vendor_elrs_wifi.sh                     # or: tools/vendor_elrs_wifi.sh path/to/elrs-crossfire-wifi-joystick-linux
   host/build.sh native test                     # the core's tests and the host's (test_wifi_live)
   python host/tests/wifi_live_test.py           # the receiver against fake modules
   ```
3. Commit `host/third_party/elrs-wifi/` (the script rewrote `VENDOR` and the Commit row above),
   `host/tests/fake_elrs.py` if it changed, and any change `input_wifi.c` needed.

The script warns and writes `commit <sha>-dirty` when the source has uncommitted changes in the files it
copies: vendor again from a commit before committing here.
