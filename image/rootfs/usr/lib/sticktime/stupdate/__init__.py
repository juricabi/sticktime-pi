"""sticktime-update: game (StickTime) and app (StickTime Pi) updates from GitHub releases.

Layout on the FAT boot partition (base = /boot/firmware/sticktime):
  sd/SCRIPTS/TOOLS/StickTime.lua      the game the host runs (reconciled from the active game slot)
  sd/SCRIPTS/TOOLS/GAME_VERSION       its version
  editor/index.html                   StickTime's simulator.html of that version, patched for the Pi
  editor/tracks/index.json            the track gallery (from the StickTime site)
  editor/sdcard/                      the newest release, staged for the emulator page's Install
  game/<gen>-<ver>-<sha8>/            game slots: immutable, complete once SLOT.json exists
  bin/ web/ lib/ VERSION SHA256SUMS   the app; bin.prev/ web.prev/ ... keep the previous one
  update/                             the updater's own files: journal, staging, records
  backup/                             the player's saves, tracks and ghosts before each game change
"""

VERSION = "1.0.0"
