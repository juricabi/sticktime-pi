# Security

## What to expect from a StickTime Pi

StickTime Pi is meant for a trusted home network or its own hotspot. Know these defaults:

- **The web page has no login.** Anyone on the same network can change settings, upload files and start
  updates. Mutating requests need an `X-StickTime: 1` header and a known `Host` name, which blocks other
  web sites (cross-site requests and DNS rebinding), not people on your network.
- **The hotspot's default password is `sticktime`.** Change `hotspot_password` in `sticktime.txt` if
  others are around.
- **SSH is off.** Turning it on (`ssh = on`) needs a password for user `pi` (`password = ...`, which is
  applied at the next boot and then removed from the file).
- The services run as root, and `sticktime.txt` on the card's FAT partition holds your WiFi password
  in plain text: anyone with the SD card can read it.
- Updates are downloaded only from GitHub over HTTPS, and their files are checked against SHA256
  checksums before they are installed.

## Reporting a vulnerability

Please don't open a public issue for a security problem. Use GitHub's private vulnerability reporting
("Report a vulnerability" under the repository's **Security** tab). Describe the problem, the version,
and how to reproduce it. You'll get an answer as soon as possible.
