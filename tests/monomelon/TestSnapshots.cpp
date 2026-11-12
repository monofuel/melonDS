// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include "NDS.h"
#include "GPU_Soft.h"

using namespace melonDS;

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); abort(); } } while (0)

namespace melonDS
{
struct SoftRendererTestAccess
{
    static void Capture(SoftRenderer& renderer, SoftLineRecord& record, u32 line)
    {
        renderer.CaptureLine(record, line);
        renderer.CaptureSprites(record, line);
    }
};
}

int main()
{
    auto nds = std::make_unique<NDS>();
    nds->Reset();
    auto renderer = std::make_unique<SoftRenderer>(*nds);
    auto records = std::make_unique<std::array<SoftLineRecord, 32>>();
    GPU& gpu = nds->GPU;
    u32 random = 0x94C8197A;
    for (u32 addr = 0; addr < sizeof(gpu.Palette); addr += 2)
    {
        gpu.WritePalette<u16>(addr, addr * 53);
        gpu.WriteOAM<u16>(addr, addr * 37);
    }
    for (u32 step = 0; step < 4096; ++step)
    {
        random = random * 1664525 + 1013904223;
        if (step % 512 < 128)
        {
            switch (step % 4)
            {
            case 0: gpu.WritePalette<u16>(0, random); break;
            case 1: gpu.WritePalette<u16>(0x400, random); break;
            case 2: gpu.WritePalette<u32>(0, random); break;
            case 3: gpu.WritePalette<u32>(0x400, random); break;
            }
        }
        else if (step % 512 >= 256)
        {
            switch (step % 16)
            {
            case 0: gpu.WritePalette<u16>(0, random); break;
            case 1: gpu.WritePalette<u16>(0x400, random); break;
            case 2: gpu.WritePalette<u32>(0, random); break;
            case 3: gpu.WritePalette<u32>(0x400, random); break;
            case 4: gpu.WritePalette<u16>(2, random); break;
            case 5: gpu.WritePalette<u32>(0x200 + (random & 0x1FC), random); break;
            case 6: gpu.WriteOAM<u32>(random & 0x7FC, random); break;
            default: break;
            }
        }
        if (step == 1024)
            nds->Reset();
        if (step == 2048)
        {
            Savestate saved;
            gpu.DoSavestate(&saved);
            saved.Finish();
            CHECK(!saved.Error);
            gpu.WritePalette<u32>(0, ~random);
            gpu.WriteOAM<u32>(0, ~random);
            auto& record = (*records)[step % records->size()];
            SoftRendererTestAccess::Capture(*renderer, record, step % 192);
            Savestate loaded(saved.Buffer(), saved.Length(), false);
            gpu.DoSavestate(&loaded);
            CHECK(!loaded.Error);
        }
        if (step == 3072)
        {
            renderer = std::make_unique<SoftRenderer>(*nds);
            records = std::make_unique<std::array<SoftLineRecord, 32>>();
        }
        auto& record = (*records)[step % records->size()];
        SoftRendererTestAccess::Capture(*renderer, record, step % 192);
        CHECK(memcmp(record.Palette, gpu.Palette, sizeof(record.Palette)) == 0);
        CHECK(memcmp(record.OAM, gpu.OAM, sizeof(record.OAM)) == 0);
    }
}
