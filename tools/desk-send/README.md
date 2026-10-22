# Desk send (2026-10-22)

Pixel JamesDSP output tap -> UDP -> Oppo desk speaker, mixed in the Oppo's pulseaudio
next to comrade's stream. SCOPE.md (JamesDSP-src) entry 2026-10-22 ~23:45.

| piece | where it runs | file here | installed as |
|---|---|---|---|
| tap ring writer | Pixel audio HAL (full variant, `tap_publish()`) | engine `jdspController.c` | `/data/data/com.termux.jdsp/files/tap.bin` |
| sender | Pixel Termux | `jdsp_tap_send.c`, `pixel-boot-70-jdsp-tap-send` | `~/bin/jdsp-tap-send`, `~/.termux/boot/70-jdsp-tap-send`, target in `~/.config/jdsp-tap-send.env` |
| receiver | Oppo Termux | `oppo-listen-pixel` | `~/bin/listen-pixel`, boot `~/.termux/boot/21-listen-pixel.sh` (twin of `20-listen-amd.sh`) |
| control | JDSP app "Desk send" card | app repo | cmd 10107 `[0, enabled, muteLocal, gainDb, 0, 0, 0]` |

Wire format = comrade's `start-oppo-usb-audio.sh` MODE=pcm: raw s16le, 48 kHz, stereo, 960-byte
packets. The sender forwards only real frames (nothing while idle/disabled); ffplay plays silence
on underrun and resumes on the next packet.
