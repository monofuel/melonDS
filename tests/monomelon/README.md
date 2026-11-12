# monoMelon renderer test

The `master` branch carries monoMelon changes based on JesseTG/melonDS commit
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

The libretro wrapper lives at https://github.com/monofuel/melonds-ds/tree/master.
Build flags, open-demo PGO training and package checks live in
https://github.com/monofuel/mono-overlay/tree/master/games-emulation/libretro-monomelon.

## Batched software 2D renderer

The threaded software renderer queues register/palette/OAM snapshots in batches
of 16 lines. Emulation keeps the original register and VRAM timing; a worker
draws from immutable records and stable flattened VRAM. Capture and raw VRAM/FIFO
display modes remain inline. State operations drain the queue and preserve window
carry. Disabling threaded rendering uses the same snapshot path serially.

The differential harness runs the actual renderer against pre-batching commit
`af1e4a005e3478be1d9327f6aedb46fd0b2e9e2a`, which must exist in the clone. It
needs a C/C++17 compiler and pthreads, with no game ROM or BIOS files:

```sh
tests/monomelon/build-batch2d.sh /tmp/melonds-batch2d-test
/tmp/melonds-batch2d-test/harness-ref unit carry raster dirty capture lifecycle boundary --seeds 0 10 --frames 4 > /tmp/batch2d-ref.txt
/tmp/melonds-batch2d-test/harness-cand unit carry raster dirty capture lifecycle boundary --seeds 0 10 --frames 4 --pace > /tmp/batch2d-cand.txt
cmp /tmp/batch2d-ref.txt /tmp/batch2d-cand.txt
```

Use a fresh output directory. `JOBS` defaults to 4; `CC`, `CXX` and `AR` support
cross compilation. `MONOMELON_BATCH2D=0` forces inline rendering; 1/8/16 select
worker batch sizes for comparisons. `--unthreaded` also disables the 3D thread.
The harness compares complete frame and GPU-state hashes across scripted
window, mosaic, palette, OAM, VRAM, capture and lifecycle changes. Allocation
poisoning and an explicit initial capture flag keep reference inputs repeatable.

These tests pass on x86-64 and the RG DS Plus. Mid-frame lifecycle fixtures keep
3D off BG0 because the reference has nondeterministic scanline-token behavior;
they do not establish correctness for that upstream issue. Small unsuppressed
ThreadSanitizer runs report existing main/3D races in both versions and no new
2D-worker race in the exercised cases. Game-level video/PCM comparisons and
performance measurements remain separate; see the overlay's performance document.

The grouped color-compositing shortcut has a separate ARM64 test:

```sh
c++ -O2 -std=c++17 -Isrc tests/monomelon/test-unblended.cpp -o /tmp/test-unblended
/tmp/test-unblended
```

It checks four million randomized pixels against scalar color effects and
exhausts flag/mode/primary-target combinations with secondary targets on/off,
ordinary/3D/sprite lower layers and window effects on/off. Only groups whose
outputs are guaranteed unchanged bypass the original scalar compositor.

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
