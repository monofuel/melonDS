// Copyright 2026 monoMelon contributors.
// Reference predicates copyright 2016-2026 melonDS team.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cassert>
#include <climits>
#include <cstdio>
#include <random>
#include "../../src/GPU3D_Soft.cpp"

using namespace melonDS;

// Original predicates from GPU3D_Soft.cpp at melonDS commit
// 09556fef981d01d05888b0a55d0a0baddda7a82d, retained independently of the new helper.
namespace reference
{
bool DepthTest_Equal_Z(s32 dstz, s32 z, u32 dstattr)
{
    s32 diff = dstz - z;
    if ((u32)(diff + 0x200) <= 0x400)
        return true;

    return false;
}

bool DepthTest_Equal_W(s32 dstz, s32 z, u32 dstattr)
{
    s32 diff = dstz - z;
    if ((u32)(diff + 0xFF) <= 0x1FE)
        return true;

    return false;
}

bool DepthTest_LessThan(s32 dstz, s32 z, u32 dstattr)
{
    if (z < dstz)
        return true;

    return false;
}

bool DepthTest_LessThan_FrontFacing(s32 dstz, s32 z, u32 dstattr)
{
    if ((dstattr & 0x00400010) == 0x00000010) // Opaque, back facing.
    {
        if (z <= dstz)
            return true;
    }
    else
    {
        if (z < dstz)
            return true;
    }

    return false;
}
}

static unsigned long long Checks = 0;

static void Check(const Polygon& polygon, s32 dstz, s32 z, u32 dstattr)
{
    bool (*referenceTest)(s32, s32, u32);
    if (polygon.Attr & (1 << 14))
        referenceTest = polygon.WBuffer ? reference::DepthTest_Equal_W : reference::DepthTest_Equal_Z;
    else if (polygon.FacingView)
        referenceTest = reference::DepthTest_LessThan_FrontFacing;
    else
        referenceTest = reference::DepthTest_LessThan;
    const DepthTest candidate(polygon);
    assert(candidate(dstz, z, dstattr) == referenceTest(dstz, z, dstattr));
    ++Checks;
}

int main()
{
    constexpr std::array<s32, 16> depths = {
        INT_MIN, INT_MIN + 1, INT_MIN + 512, -0x1000000, -513, -512, -256, -255,
        -1, 0, 1, 0xFFFFFE, 0xFFFFFF, INT_MAX - 512, INT_MAX - 1, INT_MAX};
    std::mt19937 random(0xD3F71234);
    for (unsigned mode = 0; mode < 8; ++mode)
    {
        Polygon polygon {};
        polygon.Attr = (mode & 1) ? (1 << 14) : 0;
        polygon.WBuffer = (mode & 2) != 0;
        polygon.FacingView = (mode & 4) != 0;
        for (s32 z : depths)
        {
            for (unsigned bits = 0; bits < 64; ++bits)
            {
                const u32 dstattr = (bits & 31) | ((bits & 32) << 17);
                for (s32 dstz : depths)
                    Check(polygon, dstz, z, dstattr);
                for (s32 delta = -513; delta <= 513; ++delta)
                    Check(polygon, s32(u32(z) + u32(delta)), z, dstattr);
            }
        }
        for (int i = 0; i < 1000000; ++i)
        {
            const s32 dstz = s32(random()), z = s32(random());
            const u32 dstattr = random();
            // Other polygon attributes must not affect depth-mode selection.
            polygon.Attr = (random() & ~(1u << 14)) | ((mode & 1) ? (1u << 14) : 0);
            Check(polygon, dstz, z, dstattr);
        }
    }
    printf("depth_checks=%llu failures=0\n", Checks);
}
