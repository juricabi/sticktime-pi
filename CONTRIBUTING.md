# Contributing to StickTime Pi

Thanks for helping. Reports from real hardware are the most valuable thing right now: which goggles,
VTX, radio and TV standard work, and which don't.

## Reporting a problem

Open an issue with:
- the StickTime Pi version (web page → Status, or `sticktime/VERSION` on the card) and the Pi model,
- your video setup (`video`, `tv`, `aspect` in `sticktime.txt`; goggles and VTX),
- your input (ELRS/Crossfire WiFi, USB radio, Bluetooth, gamepad) and its firmware version,
- what you expected and what happened,
- the log: web page → System → Log (remove WiFi names or anything private before posting).

Problems in the game itself (flight model, tracks, menus that look the same in the browser emulator)
belong to [StickTime](https://github.com/juricabi/sticktime/issues).

## Pull requests

1. Read [CLAUDE.md](CLAUDE.md) (the project's rules and key decisions) and [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
2. Keep changes focused; one topic per pull request.
3. Run the tests that cover your change, at least `host/build.sh native test` and
   `python host/tests/run_scenarios.py --quick`; for web, image or updater changes also their suites.
   Mention in the pull request what you ran, and what you tested on a real Pi.
4. C code: gnu99, `-Wall -Wextra` clean, no allocation in the frame loop, nothing that blocks the frame loop.
5. Drawing must stay identical to StickTime's `engine.js`; `StickTime.lua` itself is never changed here.
6. A new setting goes into `host/src/settings_keys.h` **and** `image/rootfs/usr/lib/sticktime/settings.py`
   (`python host/tests/check_settings_keys.py` checks that they match), and into `docs/USER_GUIDE.md`.

By contributing you agree that your contribution is licensed under the GPL-2.0, like the rest of the project.
