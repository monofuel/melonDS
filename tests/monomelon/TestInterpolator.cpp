// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <limits.h>
#include <random>
#include "GPU3D_Soft.h"

namespace melonDS
{
struct SoftRenderer3DTestAccess
{
    template<int direction>
    static void Run()
    {
        // The split path must match the existing unsplit SetX API.
        using Interpolator = SoftRenderer3D::Interpolator<direction>;
        std::mt19937 random(0x1A7E + direction);
        const s32 values[][2] = {
            {0, 0}, {0, 511}, {511, 0}, {-127, 513}, {513, -127},
            {0, 0xFFFFFF}, {0xFFFFFF, 0}, {INT_MIN, INT_MAX}, {INT_MAX, INT_MIN}
        };
        for (int iteration = 0; iteration < 100000; iteration++)
        {
            s32 x0 = random(), x1 = random(), w0 = random(), w1 = random();
            if (iteration % 8 == 0) x1 = x0;
            if (iteration % 8 == 1) x1 = x0 + 1;
            if (iteration % 8 == 2) x1 = x0 - 1;
            if (iteration % 4 == 0) w0 = w1 = 0;
            if (iteration % 4 == 1) w0 = w1 = 0x1000;
            if (iteration % 4 == 2) w0 = w1 = 0x1001;
            for (bool wbuffer : {false, true})
            {
                Interpolator original(x0, x1, w0, w1, wbuffer);
                Interpolator split(x0, x1, w0, w1, wbuffer);
                const s32 positions[] = {x0, x1, x0 - 1, x1 + 1, 0, -1, 255, 256, (s32)random()};
                for (int index = 0; index < 9; index++)
                {
                    original.SetX(positions[index]);
                    split.SetXForDepth(positions[index]);
                    for (const auto& pair : values)
                        assert(original.InterpolateZ(pair[0], pair[1]) == split.InterpolateZ(pair[0], pair[1]));
                    // Rejected pixels must not prepare attributes, including consecutive rejections.
                    if ((iteration + index) % 3 == 0) continue;
                    split.PrepareAttributes();
                    for (const auto& pair : values)
                    {
                        assert(original.Interpolate(pair[0], pair[1]) == split.Interpolate(pair[0], pair[1]));
                        assert(original.InterpolateZ(pair[0], pair[1]) == split.InterpolateZ(pair[0], pair[1]));
                    }
                }
            }
        }
    }
};
}

int main()
{
    melonDS::SoftRenderer3DTestAccess::Run<0>();
    melonDS::SoftRenderer3DTestAccess::Run<1>();
}
