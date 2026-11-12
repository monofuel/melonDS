/*
    Copyright 2026 monoMelon contributors

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "types.h"

namespace melonDS
{

// Per-scanline lists of polygon indices, stored as compressed rows.
// A polygon is active on row y when
//   y >= YTop && (y < YBottom || (y == YTop && YBottom == YTop)),
// and each row lists its polygons in ascending index order.
struct ActivePolygonRows
{
    static constexpr int NumRows = 192;
    static constexpr int MaxPolygons = 2048;

    u32 Offset[NumRows + 1];
    u16 Index[NumRows * MaxPolygons];

    // ytop(i) and ybottom(i) return the s32 Y range of polygon i.
    template <typename YTop, typename YBottom>
    void Build(int npolys, YTop ytop, YBottom ybottom)
    {
        u32 count[NumRows] = {};

        for (int i = 0; i < npolys; i++)
        {
            s32 first, end;
            if (RowRange(ytop(i), ybottom(i), first, end))
                for (s32 y = first; y < end; y++)
                    count[y]++;
        }

        Offset[0] = 0;
        for (int y = 0; y < NumRows; y++)
        {
            Offset[y + 1] = Offset[y] + count[y];
            count[y] = Offset[y];
        }

        for (int i = 0; i < npolys; i++)
        {
            s32 first, end;
            if (RowRange(ytop(i), ybottom(i), first, end))
                for (s32 y = first; y < end; y++)
                    Index[count[y]++] = (u16)i;
        }
    }

    // Clamped half-open row range [first, end); false when no row is active.
    static bool RowRange(s32 ytop, s32 ybottom, s32& first, s32& end)
    {
        s64 lo = ytop;
        s64 hi = (ybottom == ytop) ? (s64)ytop + 1 : (s64)ybottom;
        if (lo < 0) lo = 0;
        if (hi > NumRows) hi = NumRows;
        if (lo >= hi) return false;
        first = (s32)lo;
        end = (s32)hi;
        return true;
    }
};

}
