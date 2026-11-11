# monoMelon renderer test

The `monomelon` branch starts at JesseTG/melonDS commit
`16f127dbb587a73371827ce79689c5d237c4b59b`. It adds the existing ARM64 NEON
background-tile path and conservative sprite-priority mask from mono-overlay
`libretro-monomelon-1.4.0-r1`. The engine and these changes retain GPL-3.0-or-later.

On a little-endian ARM64 machine, run from the repository root:

```sh
c++ -O2 -std=c++17 -Isrc tests/monomelon/test-text-neon.cpp -o /tmp/test-text-neon
/tmp/test-text-neon
```

The test compares 6.4 million pixels against scalar reference arithmetic.
Cross-compilation with an ARM64 compiler and execution under QEMU is also
supported. This tests the tile helper; full-engine video/audio comparisons are
still needed for scrolling, VRAM, sprite integration and new optimizations.

The libretro wrapper lives at https://github.com/monofuel/melonds-ds/tree/monomelon.
Build flags, open-demo PGO training and package checks live in
https://github.com/monofuel/mono-overlay/tree/master/games-emulation/libretro-monomelon.
