#!/usr/bin/env bash
# Copyright 2026 monoMelon contributors.
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the melonDS core from <engine>/src as a static library and links TestBands3D.
# Usage: build-bands3d.sh <engine-dir> <new-out-dir> [extra compiler flags...]
# CXX, CC and AR may be overridden for cross builds.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
engine=$(realpath "${1:?engine dir}")
out=$(realpath -m "${2:?out dir}")
shift 2
extra=("$@")
cxx=${CXX:-g++}; cc=${CC:-gcc}; ar=${AR:-ar}; jobs=${JOBS:-4}
sources=(
    ARCodeFile.cpp ARDatabaseDAT.cpp AREngine.cpp ARM.cpp ARMInterpreter.cpp
    ARMInterpreter_ALU.cpp ARMInterpreter_Branch.cpp ARMInterpreter_LoadStore.cpp
    CP15.cpp CRC32.cpp DMA.cpp DMA_Timings.cpp FATIO.cpp FATStorage.cpp GBACart.cpp
    GBACartMotionPak.cpp GPU.cpp GPU_Soft.cpp GPU2D.cpp GPU2D_Soft.cpp GPU3D.cpp
    GPU3D_Soft.cpp GPU3D_Texcache.cpp Mic.cpp NDS.cpp NDSCart.cpp ROMList.cpp
    FreeBIOS.cpp RTC.cpp Savestate.cpp SPI.cpp SPI_Firmware.cpp SPU.cpp Utils.cpp
    Wifi.cpp WifiAP.cpp
    DSi.cpp DSi_AES.cpp DSi_Camera.cpp DSi_DSP.cpp DSi_I2C.cpp DSi_I2S.cpp DSi_NAND.cpp
    DSi_NDMA.cpp DSi_NWifi.cpp DSi_SD.cpp DSi_SPI_TSC.cpp
    DSP_HLE/UcodeBase.cpp DSP_HLE/AACUcode.cpp DSP_HLE/G711Ucode.cpp DSP_HLE/GraphicsUcode.cpp
    teakra/src/ahbm.cpp teakra/src/apbp.cpp teakra/src/btdmp.cpp teakra/src/disassembler.cpp
    teakra/src/dma.cpp teakra/src/timer.cpp teakra/src/memory_interface.cpp teakra/src/mmio.cpp
    teakra/src/parser.cpp teakra/src/processor.cpp teakra/src/teakra.cpp
    NDSCart/CartCommon.cpp NDSCart/CartRetail.cpp NDSCart/CartRetailNAND.cpp
    NDSCart/CartRetailIR.cpp NDSCart/CartRetailBT.cpp NDSCart/CartSD.cpp
    NDSCart/CartHomebrew.cpp NDSCart/CartR4.cpp
    fatfs/ff.c fatfs/ffsystem.c fatfs/ffunicode.c
    sha1/sha1.c tiny-AES-c/aes.c xxhash/xxhash.c blip-buf/blip_buf.c
)
mkdir "$out"
cp -a "$engine/src" "$out/src"
sed -e 's/${melonDS_HOMEPAGE_URL}//' -e 's/${melonDS_VERSION}/test/' \
    -e 's/${MELONDS_VERSION_SUFFIX}//' "$out/src/version.h.in" > "$out/src/version.h"
mkdir "$out/obj"
cflags=(-O2 -g -fwrapv -fno-strict-aliasing -pthread "${extra[@]}")
pids=()
for source in "${sources[@]}"; do
    object="$out/obj/${source//\//_}.o"
    case "$source" in
        *.c) "$cc" -c "${cflags[@]}" "$out/src/$source" -o "$object" & ;;
        *) "$cxx" -c -std=gnu++17 -Wno-invalid-offsetof "${cflags[@]}" -I"$out/src/teakra/include" "$out/src/$source" -o "$object" & ;;
    esac
    pids+=($!)
    if (( ${#pids[@]} >= jobs )); then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
done
for pid in "${pids[@]}"; do wait "$pid"; done
"$ar" rcs "$out/libcore.a" "$out"/obj/*.o
"$cxx" -std=gnu++17 -Wall -Wno-invalid-offsetof "${cflags[@]}" -I"$out/src" -I"$out/src/teakra/include" \
    "$here/TestBands3D.cpp" "$here/Bands3DPlatform.cpp" "$out/libcore.a" ${LINK_FLAGS:-} -o "$out/test-bands3d"
sha256sum "$out/test-bands3d"
