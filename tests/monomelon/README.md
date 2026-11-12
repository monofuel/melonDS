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

## GLES compute feasibility probe

From the source root, with EGL/GLES development headers and libraries installed:

```sh
c++ -O2 -std=c++17 -Isrc tests/monomelon/probe-compute-gles.cpp \
  -lEGL -lGLESv2 -o /tmp/probe-compute-gles
/tmp/probe-compute-gles
```

This creates a surfaceless EGL GLES 3.1 context, reports the actual renderer and
limits, and compiles/links seven representative variants of the existing compute
shaders. It changes the GLSL version, supplies ES precision declarations, enables
the supported implicit-conversion/texture-buffer extensions, and makes the two
depth early-return integer casts explicit. The shader algorithms are unchanged.

All seven variants pass on the RG DS Plus's Mali-G52/Panfrost, Mesa 26.1.8.
The probe does **not** dispatch GPU work, run a game, test pixel accuracy or
measure emulation speed. It does not provide a GLES renderer or libretro hardware
context support. Those remain separate integration and validation work.
