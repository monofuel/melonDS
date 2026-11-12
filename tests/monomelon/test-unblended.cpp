/*
    Copyright 2026 monoMelon contributors
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include <cstdio>
#include <initializer_list>
#include "GPU_ColorOp.h"
#include "GPU_ColorOpNeon.h"
using namespace melonDS;

static u32 state = 0x91B3C457;
static u32 next()
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// Scalar ColorComposite behavior across all four regular effect modes.
static u32 reference(u32 top, u32 bottom, u32 blendCnt, u32 eva, u32 evb, u32 evy, u32 window)
{
    const u32 flag1 = top >> 24;
    const u32 flag2 = bottom >> 24;
    const u32 target2 = (flag2 & 0x80) ? 0x1000 : (flag2 & 0x40) ? 0x0100 : flag2 << 8;
    if ((flag1 & 0x80) && (blendCnt & target2))
    {
        if (flag1 & 0x40)
        {
            eva = flag1 & 0x1F;
            evb = 16 - eva;
        }
        return ColorBlend4(top, bottom, eva, evb);
    }
    if ((flag1 & 0x40) && (blendCnt & target2))
        return ColorBlend5(top, bottom);
    const u32 target1 = (flag1 & 0x80) ? 0x10 : (flag1 & 0x40) ? 1 : flag1;
    if ((blendCnt & target1) && (window & 0x20))
    {
        switch ((blendCnt >> 6) & 3)
        {
        case 1:
            if (blendCnt & target2) return ColorBlend4(top, bottom, eva, evb);
            break;
        case 2: return ColorBrightnessUp(top, evy, 8);
        case 3: return ColorBrightnessDown(top, evy, 7);
        }
    }
    return top;
}

int main()
{
    unsigned copied = 0;
    for (unsigned batch = 0; batch < 1000000; batch++)
    {
        u32 pixels[4];
        for (unsigned lane = 0; lane < 4; lane++)
        {
            const u32 flag = batch % 3 == 0 ? ((batch + lane) & 0x3F)
                : batch % 3 == 1 ? 0x5F : (next() & 0xFF);
            pixels[lane] = (next() & 0xFFFFFF) | (flag << 24);
        }
        const u32 blendCnt = next() & 0x3FFF;
        const u32 mode = (blendCnt >> 6) & 3;
        const u32 primaryTargets = mode ? (blendCnt & 0x3F) << 24 : 0;
        const u32 opaque3DPattern = (mode < 2 || !(blendCnt & 1)) ? 0x5F000000 : ~0U;
        const bool copy = CanCopyUnblendedPixelsNeon(vld1q_u32(pixels), primaryTargets, opaque3DPattern);
        if (copy) copied++;
        for (unsigned lane = 0; lane < 4; lane++)
        {
            const u32 bottom = next();
            const u32 eva = next() % 17;
            const u32 evb = next() % 17;
            const u32 evy = next() % 17;
            const u32 window = next();
            const u32 expected = reference(pixels[lane], bottom, blendCnt, eva, evb, evy, window);
            if (copy && pixels[lane] != expected)
            {
                std::fprintf(stderr, "Mismatch at batch %u lane %u\n", batch, lane);
                return 1;
            }
        }
    }
    if (!copied) return 1;
    for (u32 mode = 0; mode < 4; mode++)
    for (u32 targets = 0; targets < 64; targets++)
    for (u32 flag = 0; flag < 256; flag++)
    {
        const u32 top = (flag << 24) | 0x123456;
        const u32 primaryTargets = mode ? targets << 24 : 0;
        const u32 opaque3DPattern = (mode < 2 || !(targets & 1)) ? 0x5F000000 : ~0U;
        if (!CanCopyUnblendedPixelsNeon(vdupq_n_u32(top), primaryTargets, opaque3DPattern)) continue;
        for (u32 secondary : {0U, 0x3F00U})
        for (u32 bottomFlag : {4U, 0x40U, 0x80U})
        for (u32 window : {0U, 0x20U})
        {
            const u32 blendCnt = (mode << 6) | targets | secondary;
            if (reference(top, (bottomFlag << 24) | 0x234567, blendCnt, 11, 7, 13, window) != top)
            {
                std::fprintf(stderr, "Mismatch at mode %u targets %u flag %u\n", mode, targets, flag);
                return 1;
            }
        }
    }
    std::printf("PASS 4000000 reference pixels and exhaustive flag/mode/target cases, %u copied groups\n", copied);
}
