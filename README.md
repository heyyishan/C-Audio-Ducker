# audio-duck

A low-latency audio ducking daemon for Linux.

Listens on your microphone; when you speak, it smoothly reduces the volume
of your media playback. When you stop speaking, it smoothly restores it.

Uses WebRTC VAD (standalone C, no Chromium) for speech detection and
PipeWire (via wpctl) for volume control.

## Requirements

- Arch Linux (or similar with ALSA + PipeWire)
- `alsa-lib` (development headers)
- `wireplumber` (provides `wpctl`)
- `gcc` >= 13 or `clang` >= 16 (for C23 / `-std=c2x`)
- `make`

```sh
sudo pacman -S alsa-lib wireplumber gcc make git
# C-Algorithms
