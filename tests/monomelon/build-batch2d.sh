#!/usr/bin/env bash
# Copyright 2026 monoMelon contributors.
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the differential test harness twice from one melonDS core source tree:
#   ref   the pinned pre-batching engine.
#   cand  the current working tree, including uncommitted source changes.
# Both builds use the same compiler settings and deterministic test inputs.
# Usage: build-batch2d.sh <new-out-dir> [extra compiler flags...]
# CXX, CC and AR may be overridden for cross builds. PUBLIC_SRC is only read, never written.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(realpath -m "${1:?out dir}")
shift
extra=("$@")
commit=af1e4a005e3478be1d9327f6aedb46fd0b2e9e2a
public=${PUBLIC_SRC:-$(git -C "$here" rev-parse --show-toplevel)}
cxx=${CXX:-g++}
cc=${CC:-gcc}
ar=${AR:-ar}
jobs=${JOBS:-4}

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
mkdir -p "$out/ref" "$out/cand"
git -C "$public" archive "$commit" src | tar -x -C "$out/ref"
rm -rf "$out/ref/src/frontend" "$out/ref/src/net"
sed -e 's/${melonDS_HOMEPAGE_URL}//' -e 's/${melonDS_VERSION}/test/' \
    -e 's/${MELONDS_VERSION_SUFFIX}//' "$out/ref/src/version.h.in" > "$out/ref/src/version.h"
cp -a "$out/ref/src" "$out/cand/src"
cp -a "$public/src/." "$out/cand/src/"

compile_one() {
    variant=$1 source=$2
    object="$out/$variant/obj/${source//\//_}.o"
    case "$source" in
        *.c) "$cc" -c "${cflags[@]}" "$out/$variant/src/$source" -o "$object" ;;
        *) "$cxx" -c -std=gnu++17 -Wno-invalid-offsetof "${cflags[@]}" $(includes "$variant") "$out/$variant/src/$source" -o "$object" ;;
    esac
}

cflags=(-O2 -g -fwrapv -fno-strict-aliasing -pthread "${extra[@]}")
includes() { printf -- "-I%s\n" "$out/$1/src/teakra/include"; }
for variant in ref cand; do
    mkdir -p "$out/$variant/obj"
    pids=()
    for source in "${sources[@]}"; do
        compile_one "$variant" "$source" &
        pids+=($!)
        if (( ${#pids[@]} >= jobs )); then
            wait "${pids[0]}"
            pids=("${pids[@]:1}")
        fi
    done
    for pid in "${pids[@]}"; do wait "$pid"; done
    "$ar" rcs "$out/$variant/libcore.a" "$out/$variant"/obj/*.o
    define=()
    [[ $variant == cand ]] && define=(-DBATCH2D_CANDIDATE)
    "$cxx" -std=gnu++17 -Wno-invalid-offsetof "${cflags[@]}" "${define[@]}" -I"$out/$variant/src" \
        "$here/TestBatch2D.cpp" "$here/Batch2DPlatform.cpp" "$out/$variant/libcore.a" \
        ${LINK_FLAGS:-} -o "$out/harness-$variant"
done
sha256sum "$out/harness-ref" "$out/harness-cand"
