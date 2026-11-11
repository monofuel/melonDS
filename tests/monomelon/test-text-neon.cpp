// Copyright 2026 mono-overlay contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <initializer_list>
#include "GPU_TextNeon.h"
using namespace melonDS;
static u32 state = 0x3981ABC;
static u32 next() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
int main()
{
    u16 palette[256];
    u8 bytes[8], window[8];
    u32 upper[8], lower[8], expectedUpper[8], expectedLower[8];
    for (int test = 0; test < 50000; ++test)
    {
        for (auto& color : palette) color = next();
        for (auto& pixel : bytes) pixel = next();
        for (auto& mask : window) mask = next();
        for (bool eightBit : {false, true}) for (bool flipped : {false, true}) for (u32 bg = 0; bg < 4; ++bg)
        {
            for (int i = 0; i < 8; ++i)
            {
                upper[i] = expectedUpper[i] = next();
                lower[i] = expectedLower[i] = next();
                u32 x = flipped ? 7 - i : i;
                u8 index = eightBit ? bytes[x] : (bytes[x >> 1] >> ((x & 1) * 4)) & 15;
                if (index && (window[i] & (1 << bg)))
                {
                    u16 color = palette[index];
                    u32 r = (color & 31) << 1;
                    u32 g = ((color & 0x3E0) >> 4) | (color >> 15);
                    u32 b = (color & 0x7C00) >> 9;
                    expectedLower[i] = upper[i];
                    expectedUpper[i] = r | (g << 8) | (b << 16) | (0x01000000u << bg);
                }
            }
            DrawTextTileNeon(upper, lower, window, bytes, palette, eightBit, flipped, bg);
            for (int i = 0; i < 8; ++i)
                if (upper[i] != expectedUpper[i] || lower[i] != expectedLower[i])
                { std::fprintf(stderr, "Mismatch %d/%d\n", test, i); return 1; }
        }
    }
    std::puts("PASS 6400000 exact tile pixels");
}
