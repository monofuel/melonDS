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
of 8 lines. Emulation keeps the original register and VRAM timing; a worker
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

## Scheduler, snapshots and AArch64 dispatch

These standalone fixtures build the real engine without a graphical frontend,
ROMs or BIOS files:

```sh
cmake -S tests/monomelon -B /tmp/monomelon-regressions -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/monomelon-regressions -j4
ctest --test-dir /tmp/monomelon-regressions --output-on-failure
```

The scheduler fixture compares cached deadlines with a full scan across
cancellation, callback rescheduling and reentrant deadline queries, sleep,
reset, state loading and 20,000 transitions. The snapshot fixture compares all
palette/OAM bytes through reused slots, including backdrop-only 16/32-bit
writes and lifecycle changes. Palette/OAM snapshots remain immutable while
queued; generation counters only avoid recopying unchanged bytes.

On AArch64 with JIT enabled, the dispatch fixture executes the actual generated
tails for both CPUs and ARM/Thumb modes. It checks cycles, flags, changed
deadlines, IRQ handoff, invalidated/wrong-tag entries, region exits and cache
reset against the original exit path. The shared tails preserve block boundaries
and use the normal tagged lookup table. `MONOMELON_JIT_DISPATCH=0` selects the
original exit path for comparisons; the default is enabled on AArch64.

The same CMake project also runs the 120-frame 3D/lifecycle fixture. Test
assertions stay enabled in release builds. Cross builds accept a normal CMake
toolchain file; execution under QEMU additionally needs the target runtime and
`CMAKE_CROSSCOMPILING_EMULATOR`. These fixtures complement game video/PCM checks
and performance measurements; they do not prove accuracy for every game.

## Parallel software 3D renderer

The threaded software renderer divides non-shadow frames into three horizontal
bands. Its existing thread draws rows 0–95; two persistent helpers draw 96–159
and 160–191 into private buffers. Each publishes completed rows for the 2D
consumer. Edge marking includes neighbour rows, and completed buffers are joined
before the next frame can change rendering state. Shadow and shadow-mask frames
keep a single worker because stencil history crosses scanlines.

Ordered per-row polygon lists avoid testing polygons against rows they never
reach. Rasterization and colour arithmetic are unchanged. Reset, state loading,
rendering-off writes and destruction wait for every pending worker.

The standalone tests need no ROM or BIOS files:

```sh
tests/monomelon/build-bands3d.sh . /tmp/melonds-bands3d-test
/tmp/melonds-bands3d-test/test-bands3d 120
c++ -O2 -std=c++17 -Isrc tests/monomelon/TestActivePolygons.cpp -o /tmp/test-active-polygons
/tmp/test-active-polygons
```

The band test compares serial, threaded single-worker, two-worker and three-worker
results: all output rows, both colour/depth/attribute layers, stencil and shadow
state. It covers band boundaries, fog, antialiasing, clear images, W/Z buffering,
shadow sequences, identical frames and real GPU power/reset/save-load paths.
The list test checks the original row predicate, order, extreme ranges and
maximum capacity. Host race-detector checks and native ARM64 tests provide
additional coverage; they do not prove every game and hardware transition.

Diagnostic controls are read when the renderer is constructed:

- `MONOMELON_3D_BANDS=0` retains a single 3D worker.
- `MONOMELON_3D_WORKERS=2` selects two bands; 3 is the default.
- `MONOMELON_3D_BAND_SPLIT=N` selects the two-worker split, from 1 through 191.
- `MONOMELON_3D_BAND_SPLITS=A,B` selects three-worker boundaries, with
  `1 <= A < B <= 191`.

Disabling threaded rendering retains the serial path. The three-worker default
targets the quad-core handheld; performance on other CPUs needs measurement.
See the overlay performance document for saved-scene output comparisons and
player measurements. Full-speed acceptance remains incomplete.

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
