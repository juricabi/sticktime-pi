/* The known settings: key, kind, default, choices, live, reboot, help.
 * Included by settings.c only. Keep image/rootfs/usr/lib/sticktime/settings.py in step
 * (tests/settings_vectors.txt checks both parsers against the same cases). */

/*  key                 kind       default        choices                                              live   reboot help */
#define SETTINGS_KEY_LIST \
  X("video",            SK_ENUM,   "av",          "av|hdmi",                                           false, true,  "Video output: av (composite on the 3.5 mm jack, to a VTX) or hdmi") \
  X("tv",               SK_ENUM,   "ntsc",        "ntsc|pal|pal60|ntsc-j|pal-m|pal-n|ntsc-443|secam",  false, true,  "TV standard of the composite output") \
  X("aspect",           SK_ENUM,   "4:3",         "4:3|16:9",                                          false, false, "Shape of the picture in your goggles or screen") \
  X("av_mode",          SK_ENUM,   "progressive", "interlaced|progressive",                            false, false, "Composite: interlaced (480i/576i) or progressive (240p/288p)") \
  X("av_detail",        SK_ENUM,   "high",        "high|normal",                                       false, false, "Composite: high draws twice the lines and the Pi scales them down (smoother edges, sharper text), normal draws the mode's lines") \
  X("av_flicker",       SK_ENUM,   "full",        "off|light|full",                                    false, false, "Interlaced composite: the flicker filter blends each line with its neighbours (light, or full: no flicker of thin lines)") \
  X("hdmi_mode",        SK_STRING, "auto",        "",                                                  false, false, "HDMI mode: auto (1280x720 if offered) or WxH, WxH@Hz") \
  X("margin",           SK_INT,    "0",           "0|20",                                              false, false, "Black border in percent, for goggles that cut off the edges") \
  X("fonts",            SK_ENUM,   "auto",        "auto|std|sml|lrg|pc|hd",                            false, false, "Text size: auto, or the fonts of a radio screen class") \
  X("show_ip",          SK_ENUM,   "auto",        "auto|on|off",                                       true,  false, "Show the web address on the main menu") \
  X("drawing",          SK_ENUM,   "multi",       "single|multi|gpu",                                  true,  false, "Drawing the picture: multi (on all the Pi's cores: the same picture, sooner), gpu (the graphics chip: fastest; edges may differ by a pixel) or single (one core)") \
  X("wifi_ssid",        SK_STRING, "",            "",                                                  false, false, "Your WiFi network (2.4 GHz is best next to 5.8 GHz video)") \
  X("wifi_password",    SK_SECRET, "",            "",                                                  false, false, "WiFi password (empty for an open network)") \
  X("wifi_country",     SK_STRING, "US",          "",                                                  false, false, "Two-letter country code for WiFi (US, GB, DE, HR, ...)") \
  X("hotspot",          SK_ENUM,   "auto",        "auto|on|off",                                       false, false, "Own WiFi network: auto (when no known network is found), on, off") \
  X("hotspot_ssid",     SK_STRING, "StickTime",   "",                                                  false, false, "Name of the hotspot network") \
  X("hotspot_password", SK_SECRET, "sticktime",   "",                                                  false, false, "Hotspot password (8-63 characters)") \
  X("input",            SK_ENUM,   "auto",        "auto|wifi|usb|bluetooth|gamepad|joystick|keyboard", true,  false, "Stick source: auto (the last one that moved) or a fixed one") \
  X("channel_order",    SK_STRING, "AETR",        "",                                                  true,  false, "Which channels are aileron, elevator, throttle, rudder") \
  X("stick_channels",   SK_STRING, "",            "",                                                  true,  false, "Channel of each stick: aileron, elevator, throttle, yaw, e.g. 1 2 3 4 (empty: the channel order)") \
  X("invert",           SK_STRING, "",            "",                                                  true,  false, "Sticks to invert, e.g. ail, thr") \
  X("pause_channel",    SK_INT,    "5",           "0|16",                                              true,  false, "Switch channel that pauses the game in flight (0 = off)") \
  X("stick_menus",      SK_BOOL,   "on",          "",                                                  true,  false, "Fly the menus with the sticks") \
  X("wifi_module_ip",   SK_STRING, "",            "",                                                  true,  false, "ELRS/Crossfire WiFi module IP (empty: find it by its broadcast)") \
  X("wifi_interval_us", SK_INT,    "4000",        "1000|100000",                                       true,  false, "WiFi joystick packet interval in microseconds (4000 = 250 Hz)") \
  X("bluetooth",        SK_BOOL,   "on",          "",                                                  false, false, "Bluetooth: ELRS BLE joysticks and gamepads pair automatically (off: no Bluetooth device can connect, and steadier WiFi)") \
  X("latency",          SK_STRING, "0",           "",                                                  true,  false, "Camera prediction in seconds (STICKTIME_LAT): 0 = off") \
  X("stick_mode",       SK_INT,    "2",           "1|4",                                               true,  false, "Your radio's stick mode (what getStickMode() tells the game)") \
  X("script",           SK_STRING, "/SCRIPTS/TOOLS/StickTime.lua", "",                                  true,  false, "The tool script to run (SD path)") \
  X("stats",            SK_BOOL,   "off",         "",                                                  true,  false, "Show game and display times next to the fps (game setting Show FPS)") \
  X("sd_write",         SK_ENUM,   "game",        "game|tracks|all|off",                               true,  false, "What the web page may write in the game's SD folder") \
  X("sound",            SK_BOOL,   "on",          "",                                                  true,  false, "Motor sound and beeps") \
  X("volume",           SK_INT,    "70",          "0|100",                                             true,  false, "Sound volume in percent") \
  X("music",            SK_BOOL,   "off",         "",                                                  true,  false, "Music") \
  X("music_volume",     SK_INT,    "60",          "0|100",                                             true,  false, "Music volume in percent") \
  X("music_source",     SK_ENUM,   "generated",   "generated|songs",                                   true,  false, "Music: generated live, or your songs (the music folder)") \
  X("audio_out",        SK_ENUM,   "auto",        "auto|jack|hdmi|off",                                false, false, "Audio output: the 3.5 mm jack (VTX audio pad), HDMI, or off") \
  X("audio_mono",       SK_BOOL,   "off",         "",                                                  true,  false, "Mix the sound to mono (a VTX audio pad has one channel)") \
  X("vtx",              SK_ENUM,   "off",         "off|auto|smartaudio|tramp",                         true,  false, "VTX control: off, auto (finds SmartAudio or Tramp), smartaudio, tramp") \
  X("vtx_port",         SK_ENUM,   "gpio4",       "gpio4|gpio17|gpio22|gpio23|gpio27|uart3|uart4|uart5",  true,  false, "VTX control pin: gpio4 (pin 7) or another single pin; uart3, uart4, uart5: two pins (needs a reboot)") \
  X("vtx_band",         SK_STRING, "",            "",                                                  true,  false, "VTX band: its letter in the VTX table (A, B, E, F, R, ...); empty = leave it") \
  X("vtx_channel",      SK_INT,    "1",           "1|8",                                               true,  false, "VTX channel in the band, 1-8") \
  X("vtx_freq",         SK_INT,    "0",           "0|6200",                                            true,  false, "VTX frequency in MHz instead of band and channel (0 = band and channel)") \
  X("vtx_power",        SK_INT,    "0",           "0|8",                                               true,  false, "VTX power level in the VTX table, 1 = the lowest (0 = leave it)") \
  X("vtx_pit",          SK_BOOL,   "off",         "",                                                  true,  false, "VTX pit mode: almost no power (bench, race events)") \
  X("vtx_send_only",    SK_BOOL,   "off",         "",                                                  true,  false, "Only send to the VTX (when its answers can't be read): nothing is read back") \
  X("hostname",         SK_STRING, "sticktime",   "",                                                  false, false, "Network name: http://<hostname>.local") \
  X("ssh",              SK_BOOL,   "off",         "",                                                  false, false, "SSH login as user pi (set a password first)") \
  X("password",         SK_SECRET, "",            "",                                                  false, false, "Sets the password of user pi, then is cleared") \
  X("auto_update",      SK_BOOL,   "off",         "",                                                  false, false, "Install updates of the game and StickTime Pi when online") \
  X("keep_logs",        SK_BOOL,   "off",         "",                                                  false, false, "Keep the system log across restarts (on the SD card, at most 50 MB), to see why the Pi stopped") \
  X("github_token",     SK_SECRET, "",            "",                                                  false, false, "GitHub token (optional): only for app updates from a private fork")

/* The file written when there is none (and the image's /boot/firmware/sticktime.txt). */
#define SETTINGS_TEMPLATE_TEXT \
"# StickTime Pi settings\n" \
"#\n" \
"# Edit this file on a PC (it is on the SD card) or on the web page: http://sticktime.local\n" \
"# (or the address shown on the main menu). Lines starting with # are comments.\n" \
"# Put a value in \"double quotes\" when it contains a # after a space or starts with a space.\n" \
"# Video, WiFi and system changes apply at the next boot (video changes reboot once by themselves).\n" \
"\n" \
"# ---- Video ----------------------------------------------------------------------\n" \
"# av: composite video on the 3.5 mm jack, for a 5.8 GHz VTX (or a TV's yellow input)\n" \
"# hdmi: an HDMI screen or HDMI-in goggles. The Pi 4 can't do both at once.\n" \
"video = av\n" \
"# TV standard for av: ntsc, pal, pal60, ntsc-j, pal-m, pal-n, ntsc-443, secam\n" \
"tv = ntsc\n" \
"# shape of the picture in your goggles or screen: 4:3 or 16:9\n" \
"aspect = 4:3\n" \
"# av: progressive (240p / 288p: no flicker) or interlaced (480i / 576i: twice the lines, thin lines flicker)\n" \
"av_mode = progressive\n" \
"# av: high (drawn with twice the lines, scaled down by the Pi: smoother edges, sharper text) or normal\n" \
"av_detail = high\n" \
"# interlaced av: the flicker filter blends each line with its neighbours: off, light, full (no flicker)\n" \
"av_flicker = full\n" \
"# hdmi: auto (1280x720 if the screen offers it, else its preferred mode), or e.g. 1920x1080, 1280x720@60\n" \
"hdmi_mode = auto\n" \
"# black border in percent, for goggles that cut off the edges of the picture (0-20)\n" \
"margin = 0\n" \
"# text size: auto, or the fonts of a radio screen: std (480x272), lrg (800x480), pc (1280x720), hd\n" \
"fonts = auto\n" \
"# show the web address on the main menu: auto (for a while after the network comes up), on, off\n" \
"show_ip = auto\n" \
"# drawing the picture: multi (all the Pi's cores: the same picture, sooner), gpu (the graphics chip: fastest), single (one core)\n" \
"drawing = multi\n" \
"\n" \
"# ---- WiFi -----------------------------------------------------------------------\n" \
"# your network; 2.4 GHz is best next to 5.8 GHz video (and the ELRS module is 2.4 GHz only)\n" \
"wifi_ssid =\n" \
"wifi_password =\n" \
"# two-letter country code, needed for WiFi: US, GB, DE, FR, IT, ES, HR, AU, ...\n" \
"wifi_country = US\n" \
"# own WiFi network: auto = when no known network is found, on, off\n" \
"hotspot = auto\n" \
"hotspot_ssid = StickTime\n" \
"hotspot_password = sticktime\n" \
"\n" \
"# ---- Sticks ---------------------------------------------------------------------\n" \
"# source: auto (the last one that moved), wifi, usb, bluetooth, gamepad, joystick, keyboard\n" \
"input = auto\n" \
"# which channels are aileron, elevator, throttle, rudder: AETR, TAER, ...\n" \
"channel_order = AETR\n" \
"# or the channel of each stick (CH1-CH16): aileron elevator throttle yaw, e.g. 1 2 3 4\n" \
"# (empty: channel_order decides; gamepads and the keyboard keep their own layout)\n" \
"stick_channels =\n" \
"# sticks to invert, e.g.: ail, thr\n" \
"invert =\n" \
"# switch channel that pauses the game in flight (0 = off)\n" \
"pause_channel = 5\n" \
"# fly the menus with the sticks (pitch = up/down, roll right = select, roll left = back)\n" \
"stick_menus = on\n" \
"# ELRS / Crossfire WiFi joystick: the module's IP address (empty: find it by its broadcast)\n" \
"wifi_module_ip =\n" \
"# how often the module sends the sticks, in microseconds: 4000 = 250 Hz (ELRS allows 1000 and up)\n" \
"wifi_interval_us = 4000\n" \
"# Bluetooth: on (ELRS BLE joysticks and Bluetooth gamepads pair automatically) or off (no Bluetooth device\n" \
"# can connect, and Bluetooth stops sharing the Pi's radio with WiFi)\n" \
"bluetooth = on\n" \
"\n" \
"# ---- Game -----------------------------------------------------------------------\n" \
"# camera prediction in seconds (the game's STICKTIME_LAT), 0 = off\n" \
"latency = 0\n" \
"# your radio's stick mode, 1-4 (the game shows the sticks this way)\n" \
"stick_mode = 2\n" \
"# the tool script to run, as a path in the game's SD folder (sticktime/sd)\n" \
"script = /SCRIPTS/TOOLS/StickTime.lua\n" \
"# game and display times next to the fps (turn on Show FPS in the game's settings): on, off\n" \
"stats = off\n" \
"# what the web page may change in the game's SD folder: game (tracks + installing the game), tracks, all, off\n" \
"sd_write = game\n" \
"\n" \
"# ---- Sound ----------------------------------------------------------------------\n" \
"# motor sound and beeps: on, off; volume 0-100\n" \
"sound = on\n" \
"volume = 70\n" \
"# music: on, off; volume 0-100; generated (made up live) or songs (your files in sticktime/music)\n" \
"music = off\n" \
"music_volume = 60\n" \
"music_source = generated\n" \
"# audio output: auto, jack (the 3.5 mm jack's audio, e.g. to a VTX audio pad), hdmi, off\n" \
"audio_out = auto\n" \
"# mono sound (a VTX has one audio channel): on, off\n" \
"audio_mono = off\n" \
"\n" \
"# ---- VTX control ----------------------------------------------------------------\n" \
"# change the VTX's channel, power and pit mode from the Pi over its SmartAudio or Tramp wire\n" \
"# (wiring and voltages: see the guide): off, auto (finds SmartAudio or Tramp), smartaudio, tramp\n" \
"vtx = off\n" \
"# the Pi's pin for it: gpio4 (pin 7, one wire through 1 kOhm; also gpio17, gpio22, gpio23, gpio27), or two wires\n" \
"# (the most reliable): uart3 (TX pin 7 + RX pin 29), uart4, uart5 (turning a UART on needs a reboot)\n" \
"vtx_port = gpio4\n" \
"# band: its letter in the VTX table (A, B, E, F, R, ...; empty = leave it), and the channel in it, 1-8\n" \
"vtx_band =\n" \
"vtx_channel = 1\n" \
"# frequency in MHz instead of band and channel (0 = use band and channel)\n" \
"vtx_freq = 0\n" \
"# power level in the VTX table, 1 = the lowest (0 = leave it); 25 mW is plenty next to the goggles\n" \
"vtx_power = 0\n" \
"# pit mode (almost no power): on, off\n" \
"vtx_pit = off\n" \
"# only send, read nothing back (when only the TX wire is connected): on, off\n" \
"vtx_send_only = off\n" \
"\n" \
"# ---- System ---------------------------------------------------------------------\n" \
"# network name: http://sticktime.local\n" \
"hostname = sticktime\n" \
"# SSH login as user pi: on, off (set a password first)\n" \
"ssh = off\n" \
"# sets the password of user pi at the next boot, then is cleared from this file\n" \
"password =\n" \
"# install updates of the game and StickTime Pi at boot when online: on, off\n" \
"auto_update = off\n" \
"# keep the system log across restarts (on the SD card, at most 50 MB), to see what happened\n" \
"# before the Pi stopped or restarted: on, off\n" \
"keep_logs = off\n" \
"# GitHub token (optional): only needed for StickTime Pi updates from a private fork\n" \
"github_token =\n"
