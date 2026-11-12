/*
    Copyright 2026 monoMelon contributors
    SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include <arm_neon.h>

namespace melonDS
{

// Untargeted ordinary layers retain their exact input value. The caller enables
// opaque 3D only when regular effects cannot change it. Other pixels stay scalar.
static inline bool CanCopyUnblendedPixelsNeon(uint32x4_t pixels, uint32_t primaryTargetMask,
    uint32_t opaque3DPattern)
{
    const uint32x4_t special = vandq_u32(pixels, vdupq_n_u32(0xC0000000 | primaryTargetMask));
    const uint32x4_t ordinary = vceqq_u32(special, vdupq_n_u32(0));
    const uint32x4_t opaque3D = vceqq_u32(
        vandq_u32(pixels, vdupq_n_u32(0xDF000000)), vdupq_n_u32(opaque3DPattern));
    return vminvq_u32(vorrq_u32(ordinary, opaque3D)) != 0;
}

}
