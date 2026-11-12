// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Exactness tests for the two- and three-band threaded software 3D renderer. Every banded frame is
// compared with the original single-band path (serial, and threaded with bands disabled):
// all 192 GetLine rows, both layers of the color/depth/attribute buffers, the stencil
// buffer and PrevIsShadowMask. Synthetic polygons are injected directly into the
// GPU3D render state; no ROM or game input is used.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <memory>
#include <vector>

#include "NDS.h"
#include "GPU.h"
#include "GPU_Soft.h"
#include "GPU3D_Soft.h"
#include "Savestate.h"

using namespace melonDS;

namespace melonDS
{
struct SoftRendererTestAccess
{
    static SoftRenderer3D& Soft3D(SoftRenderer& r) { return *r.Soft3D; }
};

struct SoftRenderer3DTestAccess
{
    using R = SoftRenderer3D;
    static constexpr int Words = R::BufferSize * 2;
    static constexpr int BufferSize = R::BufferSize;
    static constexpr int ScanlineWidth = R::ScanlineWidth;

    struct State
    {
        std::vector<u32> Color, Depth, Attr;
        u8 Stencil[512];
        bool PrevIsShadowMask;
    };

    static State Get(R& r)
    {
        State s;
        s.Color.assign(r.ColorBuffer, r.ColorBuffer + Words);
        s.Depth.assign(r.DepthBuffer, r.DepthBuffer + Words);
        s.Attr.assign(r.AttrBuffer, r.AttrBuffer + Words);
        memcpy(s.Stencil, r.StencilBuffer, 512);
        s.PrevIsShadowMask = r.PrevIsShadowMask;
        return s;
    }
    static void Put(R& r, const State& s)
    {
        memcpy(r.ColorBuffer, s.Color.data(), Words * 4);
        memcpy(r.DepthBuffer, s.Depth.data(), Words * 4);
        memcpy(r.AttrBuffer, s.Attr.data(), Words * 4);
        memcpy(r.StencilBuffer, s.Stencil, 512);
        r.PrevIsShadowMask = s.PrevIsShadowMask;
    }
    static void SetBands(R& r, bool enabled, int split)
    {
        r.BandsEnabled = enabled;
        r.BandWorkers = 2;
        r.BandSplit = split;
        if (r.IsThreaded())
            r.SetupRenderThread();
    }
    static void SetBands3(R& r, bool enabled, int a, int b)
    {
        r.BandsEnabled = enabled;
        r.BandWorkers = 3;
        r.BandSplits3[0] = a;
        r.BandSplits3[1] = b;
        if (r.IsThreaded())
            r.SetupRenderThread();
    }
    static void Config(const R& r, bool& enabled, int& workers, int& split, int& a, int& b)
    {
        enabled = r.BandsEnabled; workers = r.BandWorkers; split = r.BandSplit;
        a = r.BandSplits3[0]; b = r.BandSplits3[1];
    }
    static bool Banded(const R& r) { return r.FrameHelpers != 0; }
    static int FrameHelpers(const R& r) { return r.FrameHelpers; }
    static bool Pending(const R& r) { return r.FramePending; }
    static bool HasHelperThread(const R& r)
    {
        for (int k = 0; k < R::MaxBandHelpers; k++)
            if ((r.HelperThreads[k] != nullptr) != (r.BandsEnabled && k < r.BandWorkers - 1))
                return false;
        return true;
    }
    // Number of tokens left in all scanline semaphores (consumes them).
    static int LeftoverTokens(R& r)
    {
        int n = 0;
        while (Platform::Semaphore_TryWait(r.Sema_ScanlineCount, 0)) n++;
        for (auto& helper : r.Helpers)
            if (helper)
                while (Platform::Semaphore_TryWait(helper->Sema_ScanlineCount, 0)) n++;
        return n;
    }

    struct EdgeRow
    {
        s32 XL, XR;
        u32 CurVL, NextVL, CurVR, NextVR;
        s32 LenL, CovL, LenR, CovR, LenLS, CovLS, LenRS, CovRS;
        s32 WL, WR, ZL, ZR, CL, CR;
        bool operator==(const EdgeRow& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
    };

    static void LazySetup(const R& r, R::RendererPolygon* rp, s32 y)
    {
        Polygon* p = rp->PolyData;
        if (p->YTop == p->YBottom) return;
        if (y >= p->Vertices[rp->NextVL]->FinalPosition[1] && rp->CurVL != p->VBottom)
            r.SetupPolygonLeftEdge(rp, y);
        if (y >= p->Vertices[rp->NextVR]->FinalPosition[1] && rp->CurVR != p->VBottom)
            r.SetupPolygonRightEdge(rp, y);
    }

    static EdgeRow Record(R::RendererPolygon* rp)
    {
        EdgeRow e;
        memset(&e, 0, sizeof(e));
        Polygon* p = rp->PolyData;
        e.XL = rp->XL; e.XR = rp->XR;
        e.CurVL = rp->CurVL; e.NextVL = rp->NextVL; e.CurVR = rp->CurVR; e.NextVR = rp->NextVR;
        rp->SlopeL.EdgeParams<false>(&e.LenL, &e.CovL);
        rp->SlopeR.EdgeParams<false>(&e.LenR, &e.CovR);
        rp->SlopeL.EdgeParams<true>(&e.LenLS, &e.CovLS);
        rp->SlopeR.EdgeParams<true>(&e.LenRS, &e.CovRS);
        e.WL = rp->SlopeL.Interp.Interpolate(p->FinalW[rp->CurVL], p->FinalW[rp->NextVL]);
        e.WR = rp->SlopeR.Interp.Interpolate(p->FinalW[rp->CurVR], p->FinalW[rp->NextVR]);
        e.ZL = rp->SlopeL.Interp.InterpolateZ(p->FinalZ[rp->CurVL], p->FinalZ[rp->NextVL]);
        e.ZR = rp->SlopeR.Interp.InterpolateZ(p->FinalZ[rp->CurVR], p->FinalZ[rp->NextVR]);
        e.CL = rp->SlopeL.Interp.Interpolate(p->Vertices[rp->CurVL]->FinalColor[0], p->Vertices[rp->NextVL]->FinalColor[0]);
        e.CR = rp->SlopeR.Interp.Interpolate(p->Vertices[rp->CurVR]->FinalColor[0], p->Vertices[rp->NextVR]->FinalColor[0]);
        return e;
    }

    // Returns the number of mismatching rows between stepping from YTop and direct setup.
    static int CompareEdgeWalk(const R& r, Polygon* p)
    {
        auto seq = std::make_unique<R::RendererPolygon>();
        auto dir = std::make_unique<R::RendererPolygon>();
        r.SetupPolygon(seq.get(), p);
        s32 last = (p->YTop == p->YBottom) ? p->YTop : p->YBottom - 1;
        int bad = 0;
        for (s32 y = p->YTop; y <= last; y++)
        {
            LazySetup(r, seq.get(), y);
            EdgeRow a = Record(seq.get());
            r.SetupPolygon(dir.get(), p, y);
            LazySetup(r, dir.get(), y);
            EdgeRow b = Record(dir.get());
            if (!(a == b)) bad++;
            seq->XL = seq->SlopeL.Step();
            seq->XR = seq->SlopeR.Step();
        }
        return bad;
    }
};
}

using TA = SoftRenderer3DTestAccess;

namespace
{

int Failures = 0;
GPU* Gpu = nullptr;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); Failures++; } } while (0)

struct Rng
{
    u64 S;
    u32 Next() { S ^= S << 13; S ^= S >> 7; S ^= S << 17; return (u32)(S >> 11); }
    s32 Range(s32 lo, s32 hi) { return lo + (s32)(Next() % (u32)(hi - lo + 1)); }
};

struct Scene
{
    std::vector<Vertex> Verts;
    std::vector<Polygon> Polys;
    u32 DispCnt = 0;
};

enum { Shadow_None, Shadow_Mask, Shadow_Both };

void FinishPolygon(Polygon& p)
{
    u32 vtop = 0, vbot = 0;
    s32 ytop = 192, ybot = 0, xtop = 256, xbot = 0;
    for (u32 i = 0; i < p.NumVertices; i++)
    {
        Vertex* v = p.Vertices[i];
        if (v->FinalPosition[1] < ytop) { xtop = v->FinalPosition[0]; ytop = v->FinalPosition[1]; vtop = i; }
        if (v->FinalPosition[1] > ybot || (v->FinalPosition[1] == ybot && v->FinalPosition[0] > xbot))
        { xbot = v->FinalPosition[0]; ybot = v->FinalPosition[1]; vbot = i; }
    }
    p.VTop = vtop; p.VBottom = vbot; p.YTop = ytop; p.YBottom = ybot; p.XTop = xtop; p.XBottom = xbot;
}

Scene MakeScene(Rng& rng, int npolys, int shadow, s32 focusY)
{
    Scene s;
    s.Verts.resize(npolys * 4);
    s.Polys.resize(npolys);
    for (int i = 0; i < npolys; i++)
    {
        Polygon& p = s.Polys[i];
        memset(&p, 0, sizeof(p));
        int kind = rng.Range(0, 9);
        p.NumVertices = (kind < 5) ? 3 : 4;
        s32 cx = rng.Range(-20, 276), cy = rng.Range(-10, 202);
        if (rng.Range(0, 2) == 0) cy = focusY + rng.Range(-4, 4);
        s32 rx = rng.Range(1, 120), ry = rng.Range(0, 100);
        if (kind == 9) ry = 0; // single row (line polygon)
        if (kind == 8) ry = 1;
        for (u32 k = 0; k < p.NumVertices; k++)
        {
            Vertex& v = s.Verts[i * 4 + k];
            memset(&v, 0, sizeof(v));
            // roughly convex: points around a circle in angular order
            static const int ang[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
            static const int ang3[3][2] = {{0, -1}, {1, 1}, {-1, 1}};
            const int* a = (p.NumVertices == 4) ? ang[k] : ang3[k];
            s32 x = cx + a[0] * rx + rng.Range(-rx / 3, rx / 3);
            s32 y = cy + a[1] * ry + rng.Range(-ry / 3 - 1, ry / 3 + 1);
            if (kind == 9) y = cy;
            if (kind == 7) x = cx + (k & 1) * rx; // vertical edges
            v.FinalPosition[0] = std::min(std::max(x, 0), 256);
            v.FinalPosition[1] = std::min(std::max(y, 0), 192);
            for (int c = 0; c < 3; c++) v.FinalColor[c] = rng.Range(0, 0x1FF);
            v.Position[3] = 1;
            p.Vertices[k] = &v;
            p.FinalZ[k] = rng.Range(0, 0xFFFFFF);
            p.FinalW[k] = rng.Range(1, 0xFFFF);
            if (rng.Range(0, 3) == 0) p.FinalW[k] = 0x1000;
        }
        p.WBuffer = rng.Range(0, 1);
        p.FacingView = rng.Range(0, 1);
        u32 alpha = (u32)rng.Range(0, 4);
        alpha = alpha == 0 ? 0 : alpha == 1 ? (u32)rng.Range(1, 30) : 31;
        u32 mode = (u32)rng.Range(0, 2);
        u32 id = (u32)rng.Range(0, 63);
        bool sh = false;
        if (shadow != Shadow_None && rng.Range(0, 3) == 0)
        {
            mode = 3;
            sh = true;
            if (shadow == Shadow_Mask) id = 0;
            if (alpha == 31 || alpha == 0) alpha = 15;
        }
        p.Attr = (mode << 4) | (3u << 6) | (alpha << 16) | (id << 24);
        if (rng.Range(0, 1)) p.Attr |= 1u << 15; // fog
        if (rng.Range(0, 1)) p.Attr |= 1u << 11; // translucent depth write
        if (rng.Range(0, 5) == 0) p.Attr |= 1u << 14; // depth equal
        p.Translucent = alpha > 0 && alpha < 31;
        p.IsShadowMask = sh && id == 0;
        p.IsShadow = sh && id != 0;
        p.Degenerate = rng.Range(0, 40) == 0;
        FinishPolygon(p);
    }
    s.DispCnt = (u32)rng.Range(0, 1) << 5;           // edge marking
    s.DispCnt |= (u32)rng.Range(0, 1) << 4;          // antialiasing
    s.DispCnt |= (u32)rng.Range(0, 1) << 3;          // alpha blending
    s.DispCnt |= (u32)rng.Range(0, 1) << 7;          // fog
    s.DispCnt |= (u32)rng.Range(0, 1) << 1;          // toon/highlight
    s.DispCnt |= (u32)rng.Range(0, 1) << 2;          // alpha test
    if (rng.Range(0, 2) == 0) s.DispCnt |= 1u << 14; // clear image
    return s;
}

// Frames started by SetThreaded, Reset or RestartFrame render whatever RenderPolygonRAM
// points at, as with the GPU3D-owned polygon RAM, so applied scenes are never freed.
std::deque<Scene> LiveScenes;

void Apply(GPU3D& g, Rng& rng, Scene& src)
{
    LiveScenes.push_back(src);
    Scene& s = LiveScenes.back();
    for (auto& p : s.Polys)
        for (u32 k = 0; k < p.NumVertices; k++)
            p.Vertices[k] = &s.Verts[p.Vertices[k] - src.Verts.data()];
    g.RenderNumPolygons = (u32)s.Polys.size();
    for (size_t i = 0; i < s.Polys.size(); i++) g.RenderPolygonRAM[i] = &s.Polys[i];
    g.RenderDispCnt = s.DispCnt;
    g.RenderAlphaRef = (u8)rng.Range(0, 31);
    for (auto& t : g.RenderToonTable) t = (u16)rng.Next();
    for (auto& t : g.RenderEdgeTable) t = (u16)rng.Next();
    g.RenderFogColor = rng.Next();
    g.RenderFogOffset = (u32)rng.Range(0, 0x7FFF);
    g.RenderFogShift = (u32)rng.Range(0, 10);
    for (auto& t : g.RenderFogDensityTable) t = (u8)rng.Range(0, 127);
    g.RenderClearAttr1 = rng.Next();
    g.RenderClearAttr2 = rng.Next();
    g.RenderFrameIdentical = false;
    g.RenderXPos = 0;
    // Clear image rows come from the flat texture VRAM; RenderFrame only refreshes dirty
    // pages, so data written here stays in place.
    if (s.DispCnt & (1u << 14))
        for (u32 i = 0x40000; i < 0x80000; i++) Gpu->VRAMFlat_Texture[i] = (u8)rng.Next();
}

TA::State RandomState(Rng& rng)
{
    TA::State s;
    s.Color.resize(TA::Words); s.Depth.resize(TA::Words); s.Attr.resize(TA::Words);
    for (int i = 0; i < TA::Words; i++) { s.Color[i] = rng.Next(); s.Depth[i] = rng.Next() & 0xFFFFFF; s.Attr[i] = rng.Next(); }
    for (auto& b : s.Stencil) b = (u8)rng.Range(0, 1);
    s.PrevIsShadowMask = rng.Range(0, 1);
    return s;
}

struct Frame
{
    std::vector<u32> Lines;
    TA::State State;
};

Frame RenderOnce(SoftRenderer3D& r)
{
    Frame f;
    r.RenderFrame();
    f.Lines.resize(192 * 256);
    for (int y = 0; y < 192; y++)
        memcpy(&f.Lines[y * 256], r.GetLine(y, 0, false), 256 * 4);
    r.FinishRendering();
    f.State = TA::Get(r);
    return f;
}

bool SameState(const TA::State& a, const TA::State& b, const char* what, int* firstRow)
{
    *firstRow = -1;
    for (int i = 0; i < TA::Words; i++)
        if (a.Color[i] != b.Color[i] || a.Depth[i] != b.Depth[i] || a.Attr[i] != b.Attr[i])
        { *firstRow = i; break; }
    bool same = *firstRow < 0 && !memcmp(a.Stencil, b.Stencil, 512) && a.PrevIsShadowMask == b.PrevIsShadowMask;
    (void)what;
    return same;
}

struct BandCfg
{
    int Workers, A, B;
};
const BandCfg Two96{2, 96, 0};
const BandCfg Three{3, 64, 128};

void Configure(SoftRenderer3D& r, bool threaded, bool bands, const BandCfg& c)
{
    r.SetThreaded(threaded);
    if (c.Workers == 3)
        TA::SetBands3(r, bands, c.A, c.B);
    else
        TA::SetBands(r, bands, c.A);
    if (threaded)
    {
        // Drop the frame SetThreaded/EnableRenderThread started, like RestartFrame does.
        r.FinishRendering();
        r.SetupRenderThread();
    }
}

void CompareFrame(SoftRenderer3D& r, GPU3D& g, Rng& rng, Scene& scene, const TA::State& s0, const BandCfg& split, const char* name, bool expectBanded)
{
    Apply(g, rng, scene);
    Configure(r, false, false, split);
    TA::Put(r, s0);
    Frame ref = RenderOnce(r);

    Configure(r, true, false, split);
    TA::Put(r, s0);
    Frame unb = RenderOnce(r);
    CHECK(!TA::Banded(r), "%s: unbanded frame published as banded", name);
    CHECK(TA::LeftoverTokens(r) == 0, "%s: unbanded leftover tokens", name);

    Configure(r, true, true, split);
    CHECK(TA::HasHelperThread(r), "%s: helper thread missing", name);
    TA::Put(r, s0);
    Frame band = RenderOnce(r);
    CHECK(TA::Banded(r) == expectBanded, "%s: banded=%d expected %d", name, TA::Banded(r), expectBanded);
    CHECK(TA::FrameHelpers(r) == (expectBanded ? split.Workers - 1 : 0), "%s: %d helpers for %d workers", name, TA::FrameHelpers(r), split.Workers);
    CHECK(TA::LeftoverTokens(r) == 0, "%s: banded leftover tokens", name);
    CHECK(!TA::Pending(r), "%s: frame still pending", name);

    int row;
    CHECK(ref.Lines == unb.Lines, "%s: threaded unbanded lines differ from serial", name);
    CHECK(SameState(ref.State, unb.State, name, &row), "%s: threaded unbanded state differs (word %d)", name, row);
    if (ref.Lines != band.Lines)
    {
        int y = 0;
        while (y < 192 && !memcmp(&ref.Lines[y * 256], &band.Lines[y * 256], 1024)) y++;
        CHECK(false, "%s workers %d split %d,%d: banded GetLine differs first at row %d", name, split.Workers, split.A, split.B, y);
    }
    if (!SameState(ref.State, band.State, name, &row))
        CHECK(false, "%s workers %d split %d,%d: banded buffer state differs (word %d, buffer row %d)", name, split.Workers, split.A, split.B, row, row < 0 ? -1 : (row % TA::BufferSize) / TA::ScanlineWidth);
}

}

namespace melonDS { struct SoftRenderer3DTestAccess; }

int main(int argc, char** argv)
{
    int iterations = argc > 1 ? atoi(argv[1]) : 60;
    auto nds = std::make_unique<NDS>();
    auto soft = std::make_unique<SoftRenderer>(*nds);
    SoftRenderer3D& r = SoftRendererTestAccess::Soft3D(*soft);
    GPU3D& g = nds->GPU.GPU3D;
    Gpu = &nds->GPU;
    Rng rng{0x9E3779B97F4A7C15ull};

    // 1. Edge walk: direct setup at every row equals stepping from YTop.
    {
        int polys = 0, bad = 0;
        for (int it = 0; it < 400; it++)
        {
            Scene sc = MakeScene(rng, 8, Shadow_None, 96);
            for (auto& p : sc.Polys)
            {
                bad += TA::CompareEdgeWalk(r, &p);
                polys++;
            }
        }
        CHECK(bad == 0, "edge walk: %d mismatching rows", bad);
        printf("edge walk: %d polygons, %d mismatching rows\n", polys, bad);
    }

    // 2. Full frames at boundary and arbitrary splits. Each scene renders serially, threaded
    // single-band, with two bands and with three bands from the same start state, so the
    // two- and three-band results are both compared with the single-band output.
    static const int splits[] = {94, 95, 96, 97, 1, 2, 190, 191, 48, 150, 63, 64, 65, 127, 128, 129};
    static const int splits3[][2] = {{64, 128}, {63, 127}, {65, 129}, {63, 129}, {65, 127},
        {64, 65}, {127, 128}, {1, 2}, {190, 191}, {1, 191}, {96, 97}, {63, 64}, {128, 129}, {96, 160}};
    int frames = 0;
    for (int it = 0; it < iterations; it++)
    {
        int split = splits[it % (int)(sizeof(splits) / sizeof(splits[0]))];
        const int* pair = splits3[it % (int)(sizeof(splits3) / sizeof(splits3[0]))];
        BandCfg two{2, split, 0}, three{3, pair[0], pair[1]};
        int n = (it % 4 == 0) ? 1 : (it % 4 == 1) ? 7 : (it % 4 == 2) ? 60 : 300;
        Scene sc = MakeScene(rng, n, Shadow_None, (it & 1) ? pair[1] : pair[0]);
        bool anyLive = false;
        for (auto& p : sc.Polys) anyLive |= !p.Degenerate;
        TA::State s0 = RandomState(rng);
        char name[64];
        snprintf(name, sizeof(name), "frame %d (n=%d dispcnt=%04x)", it, n, sc.DispCnt);
        u64 seed = rng.S;
        Rng r1{seed};
        CompareFrame(r, g, r1, sc, s0, two, name, anyLive);
        Rng r3{seed};
        CompareFrame(r, g, r3, sc, s0, three, name, anyLive);
        rng.Next();
        frames++;
    }
    printf("frames: %d compared\n", frames);

    // 3. Zero polygons stay single-band; shadow frames fall back and keep stencil history.
    {
        Scene empty;
        TA::State s0 = RandomState(rng);
        Rng r1{rng.S};
        CompareFrame(r, g, r1, empty, s0, Two96, "zero polygons", false);
        Rng r1b{r1.S};
        CompareFrame(r, g, r1b, empty, s0, Three, "zero polygons 3", false);
        for (int it = 0; it < 12; it++)
        {
            Scene sc = MakeScene(rng, 80, it & 1 ? Shadow_Both : Shadow_Mask, 96);
            bool anyShadow = false;
            for (auto& p : sc.Polys) anyShadow |= !p.Degenerate && (p.IsShadow || p.IsShadowMask);
            Rng r2{rng.S};
            CompareFrame(r, g, r2, sc, RandomState(rng), (it & 2) ? Three : Two96, "shadow frame", !anyShadow);
        }
    }

    // 4. band -> shadow -> band sequences against a serial sequence from the same start.
    // Even sequences use three bands, odd ones two; from 4 on the last frame switches count.
    for (int seqn = 0; seqn < 8; seqn++)
    {
        const BandCfg& cfg = (seqn & 1) ? Two96 : Three;
        const BandCfg& last = seqn >= 4 ? ((seqn & 1) ? Three : Two96) : cfg;
        Scene a = MakeScene(rng, 120, Shadow_None, 96);
        Scene b = MakeScene(rng, 120, Shadow_Both, 96);
        Scene c = MakeScene(rng, 120, Shadow_None, 96);
        TA::State s0 = RandomState(rng);
        u64 seed = rng.S;
        Scene* seq[3] = {&a, &b, &c};
        std::vector<Frame> ref, band;
        Configure(r, false, false, cfg);
        TA::Put(r, s0);
        Rng ra{seed};
        for (Scene* s : seq) { Apply(g, ra, *s); ref.push_back(RenderOnce(r)); }
        Configure(r, true, true, cfg);
        TA::Put(r, s0);
        Rng rb{seed};
        for (Scene* s : seq)
        {
            if (s == &c) Configure(r, true, true, last);
            Apply(g, rb, *s);
            band.push_back(RenderOnce(r));
        }
        for (int i = 0; i < 3; i++)
        {
            int row;
            CHECK(ref[i].Lines == band[i].Lines, "sequence %d frame %d: lines differ", seqn, i);
            CHECK(SameState(ref[i].State, band[i].State, "seq", &row), "sequence %d frame %d: state differs (word %d)", seqn, i, row);
        }
        rng.Next();
    }
    printf("sequences: done\n");

    // 5. FrameIdentical after a banded frame returns the complete previous image.
    for (const BandCfg* cfg : {&Three, &Two96})
    {
        Scene sc = MakeScene(rng, 100, Shadow_None, 96);
        Configure(r, true, true, *cfg);
        Rng r1{rng.S};
        Apply(g, r1, sc);
        Frame first = RenderOnce(r);
        CHECK(TA::Banded(r), "identical: first frame not banded");
        g.RenderFrameIdentical = true;
        Frame second = RenderOnce(r);
        CHECK(!TA::Banded(r), "identical frame published as banded");
        CHECK(first.Lines == second.Lines, "identical frame lines differ");
        CHECK(TA::LeftoverTokens(r) == 0, "identical: leftover tokens");
        g.RenderFrameIdentical = false;
    }

    // 6. Lifecycle: aborted frames, restart, reset, toggles, split override and destruction.
    {
        Scene sc = MakeScene(rng, 200, Shadow_None, 96);
        Configure(r, true, true, Three);
        Rng r1{rng.S};
        Apply(g, r1, sc);
        for (int k = 0; k < 20; k++)
        {
            r.RenderFrame();
            int stop = (k * 37) % 193;
            for (int y = 0; y < stop; y++) r.GetLine(y, 0, false);
            for (int y = stop; y < 192; y++) r.GetLine(y, 0, true); // aborted rest
            r.FinishRendering();
            CHECK(!TA::Pending(r), "abort %d: still pending", k);
            r.RestartFrame();             // like GPU::FinishFrame on abort
            for (int y = 0; y < 192; y++) r.GetLine(y, 0, false);
            r.FinishRendering();
            CHECK(TA::LeftoverTokens(r) == 0, "abort %d: leftover tokens", k);
            if (k % 5 == 0) { r.Reset(); for (int y = 0; y < 192; y++) r.GetLine(y, 0, false); r.FinishRendering(); }
            if (k % 7 == 0) { r.SetThreaded(false); r.SetThreaded(true); r.FinishRendering(); r.SetupRenderThread(); }
            if (k % 3 == 0)
            {
                if (k % 2 == 0) TA::SetBands3(r, k % 4 != 2, 1 + (k * 7) % 95, 96 + (k * 11) % 96);
                else TA::SetBands(r, true, 1 + (k * 13) % 191);
            }
            CHECK(TA::HasHelperThread(r), "toggle %d: helper threads do not match the configuration", k);
        }
        // Savestate pattern: Setup (waits) then Enable.
        r.RenderFrame();
        r.SetupRenderThread();
        CHECK(!TA::Pending(r), "savestate setup left a frame pending");
        r.EnableRenderThread();
        for (int y = 0; y < 192; y++) r.GetLine(y, 0, false);
        r.FinishRendering();
        CHECK(TA::LeftoverTokens(r) == 0, "savestate: leftover tokens");
        // Destroy with a frame in flight.
        r.RenderFrame();
    }
    soft.reset();

    // 7. Environment configuration parsing.
    {
        struct EnvCase { const char *bands, *workers, *split, *splits; bool en; int w, s, a, b; };
        static const EnvCase cases[] = {
            {nullptr, nullptr, nullptr, nullptr, true, 3, 96, 96, 160},
            {"0", "3", nullptr, nullptr, false, 3, 96, 96, 160},
            {"1", "2", "80", nullptr, true, 2, 80, 96, 160},
            {nullptr, "4", "0", "50,40", true, 3, 96, 96, 160},
            {nullptr, "3", "192", "1,191", true, 3, 96, 1, 191},
            {nullptr, "2x", "191", "60,", true, 3, 191, 96, 160},
            {nullptr, nullptr, nullptr, "63,129", true, 3, 96, 63, 129},
            {nullptr, nullptr, nullptr, "0,100", true, 3, 96, 96, 160},
            {nullptr, nullptr, nullptr, "100,192", true, 3, 96, 96, 160},
        };
        for (const EnvCase& c : cases)
        {
            auto set = [](const char* n, const char* v) { if (v) setenv(n, v, 1); else unsetenv(n); };
            set("MONOMELON_3D_BANDS", c.bands);
            set("MONOMELON_3D_WORKERS", c.workers);
            set("MONOMELON_3D_BAND_SPLIT", c.split);
            set("MONOMELON_3D_BAND_SPLITS", c.splits);
            auto probe = std::make_unique<SoftRenderer>(*nds);
            bool en; int w, s, a, b;
            TA::Config(SoftRendererTestAccess::Soft3D(*probe), en, w, s, a, b);
            CHECK(en == c.en && w == c.w && s == c.s && a == c.a && b == c.b,
                "env case workers=%s splits=%s: got %d %d %d %d,%d", c.workers ? c.workers : "-", c.splits ? c.splits : "-", en, w, s, a, b);
        }
        for (const char* n : {"MONOMELON_3D_BANDS", "MONOMELON_3D_WORKERS", "MONOMELON_3D_BAND_SPLIT", "MONOMELON_3D_BAND_SPLITS"})
            unsetenv(n);
        printf("environment: done\n");
    }

    // Exercise real GPU lifecycle entry points against its installed renderer.
    nds->Reset();
    nds->GPU.CaptureEnable = false;
    auto actualSoft = std::make_unique<SoftRenderer>(*nds);
    SoftRenderer3D& actual = SoftRendererTestAccess::Soft3D(*actualSoft);
    nds->GPU.SetRenderer(std::move(actualSoft));
    for (int k = 0; k < 12; k++)
    {
        Scene scene = MakeScene(rng, 300, Shadow_None, 96);
        Rng applyRng{rng.S};
        Apply(g, applyRng, scene);
        const BandCfg& cfg = (k & 1) ? Two96 : Three;
        Configure(actual, false, false, cfg);
        TA::State initial = RandomState(rng);
        TA::Put(actual, initial);
        Frame expected = RenderOnce(actual);
        Configure(actual, true, true, cfg);
        TA::Put(actual, initial);
        actual.RenderFrame();
        nds->GPU.SetPowerCnt(0x820B);
        CHECK(!TA::Pending(actual), "power off: frame remains pending");
        std::vector<u32> lines(192 * 256);
        for (int y = 0; y < 192; y++)
            memcpy(&lines[y * 256], actual.GetLine(y, 0, false), 256 * 4);
        CHECK(lines == expected.Lines, "power off: in-flight frame changed");
        CHECK(TA::LeftoverTokens(actual) == 0, "power off: leftover tokens");

        Apply(g, applyRng, scene);
        actual.RenderFrame();
        nds->GPU.Reset();
        actual.FinishRendering();
        for (int y = 0; y < 192; y++) actual.GetLine(y, 0, false);
        CHECK(!TA::Pending(actual), "GPU reset: frame remains pending");
        CHECK(TA::LeftoverTokens(actual) == 0, "GPU reset: leftover tokens");
    }
    Savestate saved;
    nds->GPU.DoSavestate(&saved);
    saved.Finish();
    CHECK(!saved.Error, "GPU save failed");
    Savestate loaded(saved.Buffer(), saved.Length(), false);
    nds->GPU.DoSavestate(&loaded);
    CHECK(!loaded.Error, "GPU load failed");
    nds->GPU.SetPowerCnt(0x820B);
    CHECK(!TA::Pending(actual), "GPU load then power off: frame remains pending");
    for (int y = 0; y < 192; y++) actual.GetLine(y, 0, false);
    CHECK(TA::LeftoverTokens(actual) == 0, "GPU load: leftover tokens");
    nds.reset();

    printf("%s: %d failures\n", Failures ? "FAILED" : "PASSED", Failures);
    return Failures ? 1 : 0;
}
