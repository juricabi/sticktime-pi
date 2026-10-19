# StickTime Pi: user guide

StickTime Pi turns a Raspberry Pi into a "virtual quad": it runs the StickTime FPV simulator and sends
the picture to your analog goggles through a 5.8 GHz VTX on the Pi's AV jack (or to an HDMI screen). You
fly with your own radio.

> Something not working with your goggles, VTX or radio? Please report it with the log (web page → System →
> Log) at https://github.com/juricabi/sticktime-pi/issues.

> **Safety:** your radio keeps transmitting while you fly the sim. Take the props off or unplug the real
> quad's battery, or switch the radio's RF off (WiFi joystick and BLE modes do that by themselves).

## What you need

- A Raspberry Pi that runs 64-bit Raspberry Pi OS (any RAM size; for composite video one with an AV jack, like the
  Pi 4 or Pi 3), a good 5 V / 3 A supply, a microSD card of 8 GB or more.
- For goggles: an analog 5.8 GHz VTX, a 3.5 mm TRRS plug or cable, and the VTX's own power (7-12 V or 5 V).
  Or an HDMI screen / HDMI-in goggles.
- Optional, to set the VTX's channel and power from the Pi: a 1 kΩ resistor and a wire (a 4.7 kΩ one too for
  the two-wire mode; see [VTX control](#9-vtx-control-smartaudio--tramp)).
- Your radio, with one of:
  - **ExpressLRS or TBS Crossfire WiFi joystick** (the module's WiFi; same protocol as the Windows app),
  - **USB**: EdgeTX/OpenTX radio plugged into the Pi in "USB Joystick (HID)" mode,
  - **Bluetooth**: ELRS "BLE Joystick" (ESP32 TX modules), or a Bluetooth gamepad,
  - a USB gamepad or keyboard also work.

## 1. Flash the SD card

Download `sticktime-pi-<version>.img.xz` and write it with Raspberry Pi Imager ("Use custom") or
balenaEtcher. **Don't use Imager's "OS customisation"** (user, WiFi): StickTime Pi ignores it; set things in
`sticktime.txt` instead (next step).

## 2. Settings before the first boot (optional)

The SD card shows up on your PC as a drive called `bootfs`. Open **`sticktime.txt`** in a text editor.
Every setting is explained in the file. The usual ones:

```
video = av          # av (VTX / TV yellow input) or hdmi
tv = ntsc           # or pal
aspect = 4:3        # or 16:9, the shape of your goggles' picture
wifi_ssid = MyHomeWiFi
wifi_password = secret123
wifi_country = DE   # your two-letter country code (WiFi stays off without it)
```

Without WiFi settings the Pi starts its own WiFi network **StickTime** (password `sticktime`).

## 3. Wire the VTX

The Pi's 3.5 mm jack is TRRS:

| Contact | Signal | Connect to |
|---|---|---|
| tip | left audio | VTX audio pad (optional: motor sound in the goggles) |
| ring 1 | right audio | (or here; set `audio_mono = on`) |
| ring 2 | ground | VTX ground |
| sleeve | **composite video** | VTX video in |

Camcorder cables often carry video on the *red* plug. Power the VTX from its own supply (or the Pi's 5 V
pins if the VTX takes 5 V). **Attach the antenna before powering the VTX** and set it to 25 mW. Tune your
goggles to the VTX channel. To set the VTX's channel and power from the Pi instead of its button, also wire its
SmartAudio or Tramp pad: see [VTX control](#9-vtx-control-smartaudio--tramp).

**Sound in the goggles.** A VTX has one audio input, usually a microphone pad (MIC, AUD, often with AGC).
Set `audio_mono = on` (both channels carry the same mix, so either contact works) and wire:

```
Jack tip (left audio) ──[ 10 kΩ ]──[ 1 µF ]── VTX MIC / AUDIO pad      (a ceramic or film capacitor: no polarity)
Jack ring 2 (ground) ─────────────────────────── VTX ground (the same ground as the video)
```

The resistor (10-47 kΩ, a typical start, not a measured value) brings the Pi's line level (about 1 V) down towards
the millivolts a microphone gives; the capacitor keeps the small DC voltage that many mic pads carry for an electret
microphone away from the Pi. Check your VTX's manual: a line-level audio input needs neither part.
Start with `volume` at 30-40 % and raise it until the motor sound is clear without crackling; the VTX's AGC
evens out the rest.

## 4. First boot

Insert the card and power the Pi. The first boot takes a minute or two: the card is expanded and the settings
are applied. If you changed the video settings before the first boot (for example `video = hdmi` or
`tv = pal`), the Pi **restarts once by itself** to apply them; the same happens whenever you change them later.
Then the StickTime menu appears in the goggles.

The main menu shows the web address at the bottom for a minute (`http://192.168.x.y` or
`http://sticktime.local`). On the hotspot it is **http://10.42.0.1**.

## 5. Fly

- **Menus with the sticks** (goggles on): pitch up/down moves, **roll right** selects, **roll left** goes back.
  While a value is selected for editing, pitch up/down changes it. The switch on **channel 5** pauses in flight
  (change `pause_channel`). If the radio's signal stops in flight, the game pauses.
- **Keyboard**: arrows, Enter, Esc; fly with arrows (pitch/roll), A/D (yaw), W/S (throttle), Shift = full.
- **Gamepad**: mode 2 layout (change `stick_mode`), A = select, B/Start = back/pause.

### Easy on the eyes in goggles

Analog video smears strong colors, turns fine detail into rainbows, and the screen sits right in front of your
eyes. What helps:

- **Settings → Colors** (StickTime 1.9.0 and later): **Auto** is **Summer** on the AV output and
  **Classic** (the radio's bright colors) on HDMI. **Summer**, **Evening** and **Overcast** have a natural sky
  that fades into the haze at the horizon, duller grass, and calmer object and text colors: the next gate is
  the brightest thing in view. Overcast is the softest.
- On the AV output the text has a black outline (like Betaflight's OSD), readable over sky and grass alike.
- **Progressive** (`av_mode = progressive`, 240p / 288p) is the default since 0.2.5: no flicker of thin lines
  and text. With `av_detail = high` (the default) the game draws twice the lines and the Pi scales them down:
  sharper text and smoother edges, for a few ms more work per frame; `normal` draws 240p / 288p as they are.
  Interlaced (480i / 576i) has twice the lines, but thin lines and text flicker. A card from before 0.2.5
  keeps interlaced: web page → Video → Composite lines → Progressive.
- `tv = ntsc` (or `pal60`) refreshes at 60 Hz: a little less delay and smoother than PAL's 50 Hz. Most analog
  goggles show both. PAL has more lines, so slightly sharper text.
- Blacks grey, or dark areas crushed? Switch between `tv = ntsc` and `tv = ntsc-j` (the same signal without
  NTSC's 7.5 % black level step).

### The three editions

The Pi has all three editions of StickTime, the same as on a radio:

- **StickTime** (the color edition, `StickTime.lua`): the full game, made for color screens. It starts by default.
- **StickTime BW** (`StickTimeBW.lua` + the `StickTimeBW` folder): the game of the black & white radios
  (TX12 MkII, Zorro, Boxer...), drawn in a B&W radio's colors.
- **StickTime Lite** (`StickTimeLite.lua` + the `StickTimeLite` folder): the smaller B&W game of older radios
  (X7, X9D, T12...).

To switch, open the web page → **Game** → **Script** and pick one; it starts at once. (In `sticktime.txt` it is
`script = /SCRIPTS/TOOLS/StickTimeBW.lua`.) Each edition keeps its own bests and settings: `StickTime.dat`,
`StickTimeBW.dat`, and the Lite's in `StickTimeLite/data.txt`. Updates and the editor's **Install** replace the
game files of all three and never touch those, your tracks or your ghosts.

### ExpressLRS WiFi joystick
1. Put the Pi and the TX module on the same **2.4 GHz** network (the module's "Home WiFi"), or join the
   module's own network from the web page (Sticks → "Join module WiFi").
2. On the radio: ExpressLRS Lua → WiFi Connectivity → Enable WiFi (TX). The Pi finds the module by its
   broadcast within 5 s and asks it to stream 250 times a second. Set `wifi_module_ip` if your router blocks
   broadcasts.
3. **For smooth sticks the WiFi link must be steady:**
   - The most reliable setup is the Pi's own hotspot with the module joined to it: `hotspot = on`, and make
     `StickTime` (with your `hotspot_password`) the module's home network (on the module's WiFi page or in the
     ExpressLRS Configurator). There is no router in between, and the Pi never scans while it is a hotspot.
   - On your home network, give the Pi a good signal (better than about −70 dBm). With a weaker signal the Pi's
     WiFi chip scans in the background every 30 s, and a scan can pause the sticks for a moment.
   - Don't press **Scan** on the web page's WiFi tab while flying: a scan pauses the WiFi joystick for a moment.
   - Pauses of 0.25 s or more are counted on the web page (Status → Inputs → Pauses) and logged with the time
     and the Pi's WiFi signal (System → Log).

### TBS Crossfire
Enable the module's WiFi and the "VelociDrone Mobile" link; same network as the Pi. Broken TBS firmware:
XF 6.42/6.48 and WiFi 3.20 send no stick data; 6.31/6.36 and WiFi 2.17-3.10 work.

### USB
Plug the radio into a USB port, choose **USB Joystick (HID)**. Use a model with 100% weights, no expo
(the radio applies your channel order and stick mode).

### Bluetooth
ExpressLRS Lua → BLE Joystick (ESP32 modules). The Pi pairs automatically (`bluetooth = on`).
ExpressLRS 4.x (it shows as "ELRS Joystick") and 3.x ("ExpressLRS Joystick") put the channels on different
axes; the Pi knows both, so CH1-CH7 show as on the radio (CH8 usually doesn't come through: Linux drops the
second slider; CH9-CH16 arrive as buttons, on or off).

### Which channel is which stick
The web page's **Sticks** tab shows every channel of every source live: move a stick and watch its bar.

- `channel_order` (AETR, TAER, ... all 24 orders in a list) says what CH1-CH4 are. AETR fits ELRS and most radios.
- For any other layout, **Stick mapping** (`stick_channels`) gives each stick its own channel, CH1-CH16:
  aileron, elevator, throttle, yaw, e.g. `stick_channels = 1 2 3 4`. Its **Detect** button asks you to move
  each stick in turn and picks the channel that moved most; then press **Save**. Empty (the default, "By order")
  means `channel_order` decides. It works for the WiFi joystick, a USB radio and a Bluetooth radio; gamepads and
  the keyboard keep their own layout.
- `invert` reverses a stick after the mapping; the pause switch (`pause_channel`) stays the channel you set.
- **Stick source** (`input`): `auto` takes the source that moved last. A fixed source ignores all the others:
  if it is set to Bluetooth and only the WiFi joystick sends, the game gets no sticks. The page then says what is
  ignored and offers **Use auto**.

## 6. The web page

Open the address from the menu on a phone or PC on the same network:

- **Status**: frame rate, frame and stick latency numbers, input source and packet rate, versions.
- **Sticks**: live bars of every channel, input source, channel order, stick mapping (with Detect), invert,
  pause switch, WiFi module.
- **Video** (and VTX control), **WiFi** (scan and join, hotspot), **Sound** (volume, music, your songs: upload MP3/OGG/WAV/FLAC),
- **Tracks**: the StickTime track editor, your tracks c1-c8 on the Pi, the track gallery, and the emulator's
  **Install** to install or update the game on the Pi,
- **Game**: a remote keypad, restart the game, switch between the three editions (or another EdgeTX tool
  script, see [The three editions](#the-three-editions)), latency,
- **Updates**: game and StickTime Pi updates with rollback, **System**: name, SSH, password, restart, log.

## 7. Sound

The motor sound, the beeps and the music play on the jack's audio contacts (or HDMI). Music: generated
live, or your own songs from the `sticktime/music` folder (upload them on the web page or copy them from a PC).

## 8. Updates

Web page → Updates → Check. Game updates come from the StickTime releases on GitHub and update all three
editions together (color, B&W and Lite); they never touch your saves (including the Lite's
`StickTimeLite/data.txt`), tracks and ghosts. A bad update can be rolled back from the same page, again all
three together. StickTime Pi updates bring its program, the web page and its system scripts (the boot
setup, WiFi, hotspot and Bluetooth helpers, the updater itself); a few of those changes apply after the next
reboot, and the Updates page says so. `auto_update = on` installs updates at boot when the Pi is online. If you put files of your
own in an edition (a test build from a PC), automatic updates leave that edition as it is; a manual update
replaces them and keeps a copy in `sticktime/backup/`.

## 9. VTX control (SmartAudio / Tramp)

Most analog VTXes have a control pad: **SmartAudio** (TBS Unify and many others) or **Tramp** (ImmersionRC
and others). Wired to the Pi, it lets you set the VTX's band and channel (or a frequency), its power and pit
mode from the web page (Video → VTX control) or `sticktime.txt`, instead of pressing its button. Like
Betaflight, the Pi keeps the VTX set the way the settings say: if someone presses the VTX's button, the Pi sets
it back within a few seconds.

### Check the pad's voltage first

The Pi's pins work at 3.3 V and are **not 5 V tolerant**: 5 V on a pin can destroy the Pi. SmartAudio is a
3.3 V signal by TBS's spec (high 0.9-3.3 V). Tramp's level isn't documented anywhere (ExpressLRS drives Tramp
and SmartAudio VTXes straight from 3.3 V chips, so they listen to 3.3 V), and clones vary. So measure before you
connect:

1. Power the VTX (antenna on!) **without** the Pi's wires on its control pad.
2. With a multimeter, measure between the SmartAudio/Tramp pad and the VTX's ground.
   - **0-0.5 V** (or a reading that drifts): the usual input of a SmartAudio VTX. Connect it as below.
   - **3.0-3.4 V**: a 3.3 V line held high. Connect it as below.
   - **3.5 V or more** (often 5 V): **don't connect it directly.** Put a bidirectional level shifter in
     between (a small BSS138 MOSFET board): the resistor's far end (the two resistors' joined end with two wires)
     to an LV channel pin, the VTX's pad to the matching HV pin, LV to the Pi's 3.3 V (pin 1), HV to the VTX's logic voltage (5 V), GND to
     ground. Or leave VTX control off.
3. When in doubt, use the level shifter.

Power the Pi and the VTX together (from the same supply, as usual): a powered VTX on an unpowered Pi pushes
current into its pins (the resistors keep that small).

### Wiring: one wire (the usual way)

Like a flight controller, the Pi talks to the VTX over **one wire on one pin**, in both directions: pin 7 (GPIO4)
through a **1 kΩ resistor** to the VTX's SmartAudio/Tramp pad. The resistor protects the pin (it limits the current
if the VTX and the Pi drive the line at the same moment, or if the pad has more than 3.3 V) and doesn't disturb the
signal. Ground is already shared through the AV cable.

```
 Pi pin 7 (GPIO4) ──[ 1 kΩ ]───────────────────── VTX SmartAudio / Tramp pad
 Pi pin 6 (GND)   ──────────────────────────────── VTX ground (already joined by the AV cable's ground)
```

| Pi header pin | Through | To |
|---|---|---|
| 7 (GPIO4) | 1 kΩ resistor | VTX SmartAudio / Tramp pad (`vtx_port = gpio4`, the default) |
| 6, 9, 14, ... (GND) | - | VTX ground: the AV cable's ground (jack ring 2) already does this; add a wire if your VTX's video ground runs elsewhere |

- A quarter-watt resistor of any kind; solder it at the Pi's end and run one wire to the VTX.
- Pin 7 taken? Any of `gpio17` (pin 11), `gpio22` (pin 15), `gpio23` (pin 16), `gpio27` (pin 13) works the same way.
- The Pi sends and receives the serial bits in software on that pin (the timing is shown on the web page, Video →
  VTX control → Line timing). No reboot is needed to start.

### Wiring: two wires (the most reliable, if one pin gives errors)

If the web page shows timing problems or garbled answers with one pin, use one of the Pi's hardware UARTs: add a
second wire from the receive pin through a 4.7 kΩ resistor to the same pad, and set `vtx_port = uart3`. Pin 7 is
also UART3's transmit pin, so the first wire stays as it is. Linux has no one-wire UART mode: TX and RX both go to
the pad, and the software drops the echo of what it sent.

```
 Pi pin 7  (GPIO4, UART3 TX) ──[ 1 kΩ ]────┐
                                           ├───── VTX SmartAudio / Tramp pad
 Pi pin 29 (GPIO5, UART3 RX) ──[ 4.7 kΩ ]──┘
 Pi pin 6  (GND) ───────────────────────────────── VTX ground
```

| Pi header pin | Signal | Through | To |
|---|---|---|---|
| 7 (GPIO4) | UART3 TX | 1 kΩ resistor | VTX SmartAudio / Tramp pad |
| 29 (GPIO5) | UART3 RX | 4.7 kΩ resistor | the same pad (join the two resistors' far ends) |

- **A UART needs a reboot when you first select it** (the web page offers it): the Pi turns the UART on as it
  starts, and may restart once more by itself to do so.
- Other UARTs: `vtx_port = uart4` (TX pin 24 = GPIO8, RX pin 21 = GPIO9; these are the SPI pins) or `uart5`
  (TX pin 32 = GPIO12, RX pin 33 = GPIO13). All of them leave Bluetooth's UART alone.
- If the Pi hears its own echo but no answers from a VTX that has power and the right protocol, its control pin may
  be too weak to pull against 1 kΩ: try 2.2 kΩ or 4.7 kΩ in the TX wire (SmartAudio understands 0.9 V as high, so the
  VTX still hears the Pi).

### Send-only

If the VTX's answers can't be read at all (some clones), set `vtx_send_only = on` (with one wire, or only the TX wire
of a UART). The Pi then sends the settings a couple of times but can't see what the VTX is set to; set the protocol
by hand (`vtx = smartaudio` or `tramp`).

### Settings

On the web page: Video → VTX control (turn it on, pick the pin, then band, channel, power, pit mode). In
`sticktime.txt`:

```
vtx = auto            # off, auto (finds SmartAudio or Tramp), smartaudio, tramp
vtx_port = gpio4      # one pin: gpio4 (pin 7), gpio17, gpio22, gpio23, gpio27; two pins: uart3, uart4, uart5
vtx_band = R          # a band letter of the VTX table; empty = leave the frequency as it is
vtx_channel = 1       # 1-8
vtx_freq = 0          # MHz instead of band and channel (0 = use band and channel)
vtx_power = 1         # level in the VTX table, 1 = the lowest; 0 = leave it
vtx_pit = off         # pit mode: almost no power
vtx_send_only = off   # nothing is read back
```

With `auto` the Pi looks for SmartAudio, then Tramp, then common clone variants (the 0x00 byte quirks of AKK/RDQ
clones, 1 stop bit, a slow or fast VTX clock); that takes a few seconds. The web page shows what it found (protocol
and version), where the VTX is now (a green dot on the channel and power), and the Tramp's temperature.

Pit mode: SmartAudio 2.1 and Tramp switch it at once. SmartAudio 2.0 can only set the flag that starts pit mode at
the VTX's next power-up (the page says so). SmartAudio 1.0 has no pit mode.

### Testing the line (for the curious)

Over SSH, with the service stopped (`sudo systemctl stop sticktime`):
`sudo /boot/firmware/sticktime/bin/sticktime-pi --vtx-test gpio4` sends ten SmartAudio requests and prints the
answers and the timing (`--vtx-test-proto tramp` for Tramp). Without a VTX, wire pin 7 through 1 kΩ to pin 11 and
run `--vtx-loopback-test gpio4 gpio17`: it sends test patterns from one pin to the other at both protocols' speeds
and reports errors and how late the edges were. `sudo systemctl start sticktime` afterwards.

### VTX tables

The bands and power levels come from a **VTX table**. Without one the Pi uses a built-in table with bands A, B, E,
F and R (the 40 channels every VTX knows) and power levels by protocol: 25/200/500/800 mW for SmartAudio 1.0 and
2.0, the dBm levels a SmartAudio 2.1 VTX reports, 25/100/200/400/600 mW for Tramp. VTX makers publish their VTX's
table for Betaflight as a `.json` file (look on the maker's page or the Betaflight wiki's VTX table list): load it
on the web page (Video → VTX control → **Load VTX table**). It is stored as `sticktime/vtxtable.json` on the SD card
(you can copy it there from a PC too). **Download** saves the table in use; **Use default** goes back to the built-in
one. A table's power values are what the VTX gets: a level 0-3 for SmartAudio 2.0, dBm for SmartAudio 2.1, mW for
Tramp. Bands marked "factory" are the VTX's own (set by channel number); the others are set by frequency.

### Legal and safe

Which frequencies and how much power you may use depends on your country (some bands and channels are not allowed
everywhere, and many places need an amateur radio license for more than 25 mW). Next to the goggles 25 mW is plenty:
more only overloads the goggles' receiver. Never run a VTX without its antenna.

## Troubleshooting

| Problem | Try |
|---|---|
| No picture in the goggles | Check the cable (video on the sleeve), the VTX channel and power. `video = av` in sticktime.txt. Some goggles want `tv = pal`. With HDMI plugged in at boot, use `video = hdmi`. |
| Picture cut off at the edges | `margin = 5` (percent). |
| Hard on the eyes, colors smear, thin lines shimmer | The game's Settings → Colors: Summer, Evening or Overcast; `av_mode = progressive` (see [Easy on the eyes in goggles](#easy-on-the-eyes-in-goggles)). |
| Wrong shape | `aspect = 16:9` or `4:3`. |
| Can't find the web page | Look at the bottom of the main menu for the address. Phones on mobile data: turn it off. On the hotspot: http://10.42.0.1. |
| WiFi joystick not found | Same 2.4 GHz network; some routers isolate clients (turn "AP isolation" off) or block broadcasts (set `wifi_module_ip`). One client per module. |
| Sticks stop for a moment, then work again | A pause in the WiFi joystick's data: see the log (System → Log, "the stick data paused") and the tips under "ExpressLRS WiFi joystick" (the Pi's hotspot, a better signal, no WiFi scan while flying). |
| Sticks move the wrong way | `invert = ele` etc., or `channel_order` (AETR, TAER, ...). |
| A stick moves the wrong thing (yaw on throttle, ...) | Sticks → Stick mapping → **Detect**, then Save (see [Which channel is which stick](#which-channel-is-which-stick)). |
| Sticks don't move in the game although the source streams | Sticks → Stick source: a fixed source ignores the others. Use `auto`. |
| "The game stopped" | The message on screen says why; press ENTER (or roll right) to restart it. A track file that breaks the game is skipped. |
| The Pi stops or restarts by itself (no picture, gone from the network) | The Pi keeps its system log in memory only, so a restart erases why. Set `keep_logs = on` (web page → System → Keep logs) and reboot once: from then on the log of the run before a restart stays on the SD card (at most 50 MB). After it happens again, restart the Pi and read it over SSH with `journalctl -b -1`, or attach it to an issue. "Shut down" or "Reboot" on the web page shows up there as `web: poweroff` / `web: reboot`, a weak power supply as "Undervoltage detected". |
| The card shows no drive on a Windows PC | Windows gives the `bootfs` partition no drive letter when it has seen that card reader slot before without one (flashing tools remove the letter while writing). Open **Disk Management** (right-click Start), right-click the 512 MB `bootfs` partition → **Change Drive Letter and Paths** → **Add**. Windows remembers it. The other, big partition is the Linux system (ext4): Windows can't read it, and you don't need it. |
| Windows offers to "Scan and fix" the card | Harmless: the Pi was switched off by pulling the power while the card was in use. Let Windows fix it, or ignore it. |
| VTX control: "UART not on yet" | Two-wire mode: reboot once after choosing a UART for `vtx_port`. |
| VTX control: "Nothing comes back on RX" | Two-wire mode: the Pi doesn't even hear itself: check the RX wire (pin 29) and its 4.7 kΩ resistor, and that both resistors meet at the pad. Without an RX wire, use one pin (`vtx_port = gpio4`) or `vtx_send_only = on`. |
| VTX control: "in use" for the pin | Another program or overlay has that GPIO: pick another pin for `vtx_port`. |
| VTX control: Line timing shows frames "sent again", or answers come garbled | The one-pin mode's timing isn't good enough on this Pi right now: use the two-wire UART mode (above). |
| VTX control: "No answer from the VTX" | Check the wire to the right pad (SmartAudio / Tramp, not the video or audio pad), that the VTX has power, and the ground. Set the protocol by hand (`vtx = smartaudio` or `tramp`), try 2.2-4.7 kΩ in the TX wire, or send-only. Some VTXes only answer a few seconds after power-up. |
| VTX control: "garbled" | Noise or a line driven by both sides: check the resistors and the ground; measure the pad (a 5 V pad needs a level shifter). |
| VTX control: the VTX "didn't take" a setting | The VTX refuses that value (a locked VTX, a frequency or power it doesn't have): load your VTX's own table, or unlock it (Tramp race lock, see its manual). The Pi tries again when the setting or the VTX changes. |
| Everything else | Web page → System → Log. |
