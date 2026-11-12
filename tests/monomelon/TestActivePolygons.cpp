// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
// Checks ActivePolygonRows::Build against the software renderer's original
// per-row eligibility predicate, including ordering, capacity and guard bytes.

#include "GPU3D_ActivePolygons.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits>
#include <memory>
#include <random>
#include <vector>

using namespace melonDS;

namespace
{

bool OriginalEligible(s32 y, s32 ytop, s32 ybottom)
{
    return y >= ytop && (y < ybottom || (y == ytop && ybottom == ytop));
}

struct Guarded
{
    u8 Before[4096];
    ActivePolygonRows Rows;
    u8 After[4096];
};

int Failures = 0;

void Check(const Guarded& g, const std::vector<s32>& top, const std::vector<s32>& bottom, const char* name)
{
    const int n = (int)top.size();
    for (u8 b : g.Before) if (b != 0xA5) { printf("%s: front guard overwritten\n", name); Failures++; return; }
    for (u8 b : g.After) if (b != 0x5A) { printf("%s: back guard overwritten\n", name); Failures++; return; }
    if (g.Rows.Offset[0] != 0) { printf("%s: Offset[0] != 0\n", name); Failures++; return; }

    u64 total = 0;
    for (s32 y = 0; y < 192; y++)
    {
        std::vector<u16> expected;
        for (int i = 0; i < n; i++)
            if (OriginalEligible(y, top[i], bottom[i]))
                expected.push_back((u16)i);

        u32 begin = g.Rows.Offset[y], end = g.Rows.Offset[y + 1];
        if (end < begin || end - begin != expected.size()
            || (!expected.empty() && memcmp(&g.Rows.Index[begin], expected.data(), expected.size() * sizeof(u16)) != 0))
        {
            printf("%s: row %d mismatch (got %u, want %zu)\n", name, y, end - begin, expected.size());
            Failures++;
            return;
        }
        total += expected.size();
    }
    if (g.Rows.Offset[192] != total) { printf("%s: total mismatch\n", name); Failures++; }
}

void Run(Guarded& g, const std::vector<s32>& top, const std::vector<s32>& bottom, const char* name)
{
    memset(g.Before, 0xA5, sizeof(g.Before));
    memset(g.After, 0x5A, sizeof(g.After));
    // Stale contents from a previous frame must not leak into the result.
    memset(g.Rows.Offset, 0xEE, sizeof(g.Rows.Offset));
    g.Rows.Build((int)top.size(),
        [&](int i) { return top[i]; },
        [&](int i) { return bottom[i]; });
    Check(g, top, bottom, name);
}

}

int main()
{
    auto g = std::make_unique<Guarded>();
    const s32 smin = std::numeric_limits<s32>::min(), smax = std::numeric_limits<s32>::max();
    const s32 interesting[] = {smin, smin + 1, -65536, -193, -192, -2, -1, 0, 1, 2, 95, 190, 191, 192, 193, 255, 256, 65535, smax - 1, smax};

    {
        std::vector<s32> t, b;
        for (s32 a : interesting)
            for (s32 c : interesting)
                if (t.size() < ActivePolygonRows::MaxPolygons) { t.push_back(a); b.push_back(c); }
        Run(*g, t, b, "extreme-pairs");
    }

    {
        std::vector<s32> t, b;
        for (s32 y = -3; y < 196; y++) { t.push_back(y); b.push_back(y); }
        Run(*g, t, b, "single-line");
    }

    {
        std::vector<s32> t(ActivePolygonRows::MaxPolygons, smin), b(ActivePolygonRows::MaxPolygons, smax);
        Run(*g, t, b, "full-capacity");
        if (g->Rows.Offset[192] != 192u * ActivePolygonRows::MaxPolygons) { printf("full-capacity: wrong total\n"); Failures++; }
    }

    {
        std::vector<s32> t(ActivePolygonRows::MaxPolygons, 0), b(ActivePolygonRows::MaxPolygons, 0);
        Run(*g, t, b, "capacity-row0");
        t.assign(ActivePolygonRows::MaxPolygons, 191); b.assign(ActivePolygonRows::MaxPolygons, 191);
        Run(*g, t, b, "capacity-row191");
    }

    Run(*g, {}, {}, "empty");

    std::mt19937 rng(0x6d656c6f);
    std::uniform_int_distribution<int> count(0, ActivePolygonRows::MaxPolygons);
    std::uniform_int_distribution<s32> near(-40, 232);
    std::uniform_int_distribution<int> pick(0, (int)(sizeof(interesting) / sizeof(interesting[0])) - 1);
    std::uniform_int_distribution<int> mode(0, 9);
    for (int iter = 0; iter < 400; iter++)
    {
        int n = count(rng);
        std::vector<s32> t(n), b(n);
        for (int i = 0; i < n; i++)
        {
            int m = mode(rng);
            t[i] = m == 0 ? interesting[pick(rng)] : near(rng);
            if (m == 1) b[i] = t[i];
            else if (m == 2) b[i] = interesting[pick(rng)];
            else b[i] = (s32)std::clamp<s64>((s64)t[i] + (s64)(rng() % 24) - 4, std::numeric_limits<s32>::min(), std::numeric_limits<s32>::max());
        }
        char name[32];
        snprintf(name, sizeof(name), "random-%d", iter);
        Run(*g, t, b, name);
        if (Failures) break;
    }

    if (Failures) { printf("FAIL (%d)\n", Failures); return 1; }
    printf("PASS\n");
    return 0;
}
