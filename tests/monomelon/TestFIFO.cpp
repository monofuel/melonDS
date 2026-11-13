// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <memory>
#include <vector>
#include "NDS.h"
#include "GPU3D.h"

using namespace melonDS;

static std::vector<u8> State(GPU3D& gpu)
{
    Savestate state;
    gpu.DoSavestate(&state);
    state.Finish();
    assert(!state.Error);
    const auto* data = static_cast<const u8*>(state.Buffer());
    return {data, data + state.Length()};
}

#include "TestFifoValue.h"

static void Check(bool geometry, bool rendering)
{
    auto actual = std::make_unique<NDS>();
    auto reference = std::make_unique<NDS>();
    actual->Reset();
    reference->Reset();
    actual->GPU.GPU3D.SetEnabled(geometry, rendering);
    reference->GPU.GPU3D.SetEnabled(geometry, rendering);
    // Seed unused geometry RAM identically before comparing complete FIFO state.
    auto initial = State(actual->GPU.GPU3D);
    Savestate loaded(initial.data(), initial.size(), false);
    reference->GPU.GPU3D.DoSavestate(&loaded);
    assert(!loaded.Error);

    const u32 triangle[] = {
        0x10, 2, 0x15, 0x20, 0x7FFF, 0x40, 0,
        0x23, 0, 0, 0x23, 0x00100000, 0, 0x23, 0x10, 0, 0x41
    };
    auto write = [&](u32 address, u32 value)
    {
        actual->ARM9Timestamp += 2;
        reference->ARM9Timestamp += 2;
        actual->ARM9Write32(address, value);
        reference->ARM9IOWrite32(address & ~3u, value);
        assert(actual->GPU.GPU3D.Read32(0x04000600) == reference->GPU.GPU3D.Read32(0x04000600));
        assert(actual->IF[0] == reference->IF[0]);
        assert(actual->CPUStop == reference->CPUStop);
        assert(actual->ARM9Target == reference->ARM9Target);
        assert(State(actual->GPU.GPU3D) == State(reference->GPU.GPU3D));
    };
    for (u32 index = 0; index < 16 * 17; index++)
        write(0x04000400 + ((index % 16) * 4) + (index % 4), triangle[index % 17]);
    // Boundaries fall through to the ordinary register path, not the FIFO alias.
    for (u32 address : {0x040003FCu, 0x04000440u, 0x040005CCu})
        write(address, 0);
}

int main()
{
    CheckFifoValue();
    Check(true, true);
    Check(true, false);
    Check(false, true);
    Check(false, false);
}
