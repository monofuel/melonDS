// Copyright 2026 mono-overlay contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPU_TEXT_NEON_H
#define GPU_TEXT_NEON_H
#include <arm_neon.h>
#include <cstring>
#include "types.h"
namespace melonDS
{
static inline void StoreTextFourNeon(u32* upper, u32* lower,
    uint16x4_t colors, int16x4_t active, u32 flag)
{
    const auto c = vmovl_u16(colors);
    const auto red = vshlq_n_u32(vandq_u32(c, vdupq_n_u32(31)), 1);
    const auto green = vorrq_u32(vshrq_n_u32(vandq_u32(c, vdupq_n_u32(0x3E0)), 4), vshrq_n_u32(c, 15));
    const auto blue = vshrq_n_u32(vandq_u32(c, vdupq_n_u32(0x7C00)), 9);
    const auto pixel = vorrq_u32(vdupq_n_u32(flag), vorrq_u32(red,
        vorrq_u32(vshlq_n_u32(green, 8), vshlq_n_u32(blue, 16))));
    const auto mask = vreinterpretq_u32_s32(vmovl_s16(active));
    const auto previous = vld1q_u32(upper);
    vst1q_u32(lower, vbslq_u32(mask, previous, vld1q_u32(lower)));
    vst1q_u32(upper, vbslq_u32(mask, pixel, previous));
}
static inline void DrawTextTileNeon(u32* upper, u32* lower, const u8* windows,
    const u8* pixels, const u16* palette, bool eightBit, bool flipped, u32 bg)
{
    uint8x8_t indices;
    if (eightBit)
        indices = vld1_u8(pixels);
    else
    {
        u32 packed;
        std::memcpy(&packed, pixels, sizeof(packed));
        const auto bytes = vcreate_u8(packed);
        indices = vzip_u8(vand_u8(bytes, vdup_n_u8(15)), vshr_n_u8(bytes, 4)).val[0];
    }
    if (flipped) indices = vrev64_u8(indices);
    const auto active = vand_u8(vcgt_u8(indices, vdup_n_u8(0)),
        vtst_u8(vld1_u8(windows), vdup_n_u8(1u << bg)));
    if (!vmaxv_u8(active)) return;
    uint16x8_t colors;
    if (eightBit)
    {
        colors = vdupq_n_u16(0);
        colors = vsetq_lane_u16(palette[vget_lane_u8(indices, 0)], colors, 0);
        colors = vsetq_lane_u16(palette[vget_lane_u8(indices, 1)], colors, 1);
        colors = vsetq_lane_u16(palette[vget_lane_u8(indices, 2)], colors, 2);
        colors = vsetq_lane_u16(palette[vget_lane_u8(indices, 3)], colors, 3);
        colors = vsetq_lane_u16(palette[vget_lane_u8(indices, 4)], colors, 4);
        colors = vsetq_lane_u16(palette[vget_lane_u8(indices, 5)], colors, 5);
        colors = vsetq_lane_u16(palette[vget_lane_u8(indices, 6)], colors, 6);
        colors = vsetq_lane_u16(palette[vget_lane_u8(indices, 7)], colors, 7);
    }
    else
    {
        const auto table = vld2q_u8(reinterpret_cast<const u8*>(palette));
        colors = vorrq_u16(vmovl_u8(vqtbl1_u8(table.val[0], indices)),
            vshlq_n_u16(vmovl_u8(vqtbl1_u8(table.val[1], indices)), 8));
    }
    const auto mask = vmovl_s8(vreinterpret_s8_u8(active));
    StoreTextFourNeon(upper, lower, vget_low_u16(colors), vget_low_s16(mask), 0x01000000u << bg);
    StoreTextFourNeon(upper + 4, lower + 4, vget_high_u16(colors), vget_high_s16(mask), 0x01000000u << bg);
}
}
#endif
