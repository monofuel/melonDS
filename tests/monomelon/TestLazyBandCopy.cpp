// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
// Reuse the existing scenes and complete serial renderer-state comparator.
#define main ExistingBandsMain
#include "TestBands3D.cpp"
#undef main
#include <cassert>

static void Equal(const TA::State& a, const TA::State& b)
{
    int row;
    if (!SameState(a, b, "lazy state", &row))
    {
        fprintf(stderr, "lazy state mismatch word %d physical row %d\n", row, (row % TA::BufferSize) / TA::ScanlineWidth);
        abort();
    }
}

static void CompareNext(SoftRenderer3D& actual, SoftRenderer3D& serial)
{
    TA::Put(serial, TA::Get(actual));
    const Frame expected = RenderOnce(serial);
    const Frame result = RenderOnce(actual);
    assert(result.Lines == expected.Lines);
    Equal(result.State, expected.State);
}

static void DrainStarted(SoftRenderer3D& renderer)
{
    for (int y = 0; y < 192; y++) renderer.GetLine(y, 0, false);
    renderer.FinishRendering();
}

int main()
{
    auto nds = std::make_unique<NDS>();
    auto soft = std::make_unique<SoftRenderer>(*nds);
    auto serialSoft = std::make_unique<SoftRenderer>(*nds);
    auto& r = SoftRendererTestAccess::Soft3D(*soft);
    auto& serial = SoftRendererTestAccess::Soft3D(*serialSoft);
    auto& g = nds->GPU.GPU3D;
    Gpu = &nds->GPU;
    Rng rng{0x64c712be830a59fdull};
    Scene empty;
    Apply(g, rng, empty);
    Configure(r, false, false, Two96);
    Configure(serial, false, false, Two96);
    RenderOnce(r);
    const BandCfg configs[] = {{2, 1, 0}, {2, 191, 0}, {3, 96, 160}, {3, 1, 191}, {3, 1, 2}, {3, 190, 191}};
    for (const auto& cfg : configs)
    {
        std::vector<Scene> scenes;
        scenes.reserve(24);
        for (int i = 0; i < 24; i++)
        {
            scenes.push_back(MakeScene(rng, 80, Shadow_None, cfg.A));
            scenes.back().DispCnt = (scenes.back().DispCnt & ~0xB0u) |
                ((i & 1) << 4) | ((i & 2) << 4) | ((i & 4) << 5);
        }
        auto initial = RandomState(rng);
        const auto seed = rng.S;
        std::vector<Frame> reference;
        Configure(r, false, false, cfg);
        TA::Put(r, initial);
        Rng serialRng{seed};
        for (auto& scene : scenes) { Apply(g, serialRng, scene); reference.push_back(RenderOnce(r)); }
        Configure(r, true, true, cfg);
        TA::Put(r, initial);
        assert(!TA::Lazy(r));
        Rng band{seed};
        for (int i = 0; i < 24; i++)
        {
            Apply(g, band, scenes[i]);
            if (i) TA::CorruptUnownedRows(r);
            Frame actual = RenderOnce(r);
            assert(actual.Lines == reference[i].Lines);
            Equal(actual.State, reference[i].State);
            assert(TA::Lazy(r));
            // Snapshot reads cannot silently materialize or alter actual storage.
            const auto rawBefore = TA::Get(r, false);
            Equal(TA::Get(r), reference[i].State);
            Equal(TA::Get(r, false), rawBefore);
            int row;
            assert(!SameState(rawBefore, reference[i].State, "must remain lazy", &row));
        }
        const auto resolved = TA::Get(r);
        TA::Materialize(r);
        assert(!TA::Lazy(r));
        Equal(TA::Get(r, false), resolved);
        TA::Materialize(r);
        Equal(TA::Get(r, false), resolved);
    }

    Scene scene = MakeScene(rng, 100, Shadow_None, 96);
    Apply(g, rng, scene);
    Configure(r, true, true, Three);
    CompareNext(r, serial);
    assert(TA::Lazy(r));
    g.RenderFrameIdentical = true;
    CompareNext(r, serial);
    assert(!TA::Lazy(r));
    g.RenderFrameIdentical = false;
    CompareNext(r, serial);
    assert(TA::Lazy(r));

    // Change the new frame's bounds without Setup, retaining independent old owners.
    TA::SetRawBounds(r, 3, 80, 140); CompareNext(r, serial);
    TA::SetRawBounds(r, 2, 191, 0); CompareNext(r, serial);
    TA::SetRawBounds(r, 3, 1, 2); CompareNext(r, serial);
    TA::SetRawBounds(r, 3, 96, 160); CompareNext(r, serial);

    auto before = TA::Get(r);
    r.SetupRenderThread();
    assert(!TA::Lazy(r)); Equal(TA::Get(r, false), before);
    CompareNext(r, serial);
    before = TA::Get(r);
    r.StopRenderThread();
    assert(!TA::Lazy(r)); Equal(TA::Get(r, false), before);
    for (int y = 0; y < 192; y++)
        assert(!memcmp(r.GetLine(y, 0, false), before.Color.data() + (y+1)*TA::ScanlineWidth+1, 1024));
    r.RestartFrame(); DrainStarted(r);
    CompareNext(r, serial);

    // Reset discards old ownership before clearing; compare the real post-reset frame.
    TA::State zero = TA::Get(r);
    std::fill(zero.Color.begin(), zero.Color.end(), 0);
    std::fill(zero.Depth.begin(), zero.Depth.end(), 0);
    std::fill(zero.Attr.begin(), zero.Attr.end(), 0);
    zero.PrevIsShadowMask = false;
    TA::Put(serial, zero);
    const auto resetExpected = RenderOnce(serial);
    r.Reset(); DrainStarted(r);
    Equal(TA::Get(r), resetExpected.State);
    CompareNext(r, serial);

    Configure(r, false, false, Two96); CompareNext(r, serial);
    assert(!TA::Lazy(r));
    Configure(r, true, false, Two96); CompareNext(r, serial);
    assert(!TA::Lazy(r));
    Configure(r, true, true, Three); CompareNext(r, serial);
    Scene shadow = MakeScene(rng, 100, Shadow_Both, 96);
    shadow.Polys[0].Degenerate = false; shadow.Polys[0].IsShadow = true;
    Apply(g, rng, shadow); CompareNext(r, serial);
    assert(!TA::Lazy(r));
    Apply(g, rng, scene); CompareNext(r, serial);
    assert(TA::Lazy(r));
    Apply(g, rng, empty); CompareNext(r, serial);
    assert(!TA::Lazy(r));
    Apply(g, rng, scene); CompareNext(r, serial);

    // The actual save hook drains 2D, joins 3D, and must restore canonical storage.
    before = TA::Get(r);
    soft->PreSavestate();
    assert(!TA::Lazy(r)); Equal(TA::Get(r, false), before);
    TA::Put(serial, before);
    const auto saveHookExpected = RenderOnce(serial);
    soft->PostSavestate(); DrainStarted(r);
    Equal(TA::Get(r), saveHookExpected.State);
    assert(TA::Lazy(r));

    r.RenderFrame();
    for (int y = 0; y < 192; y++) r.GetLine(y, 0, y >= 77);
    r.FinishRendering();
    r.RestartFrame(); DrainStarted(r);
    CompareNext(r, serial);
    assert(TA::LeftoverTokens(r) == 0);

    // Exercise real save/load with completed lazy state. Synthetic scene pointers
    // cannot be serialized as GPU RAM indexes, so retire them after joining without
    // rendering: the buffers remain helper-owned on entry to GPU::DoSavestate.
    nds->GPU.SetRenderer(std::move(soft));
    Apply(g, rng, scene);
    Configure(r, true, true, Three);
    CompareNext(r, serial);
    assert(TA::Lazy(r));
    before = TA::Get(r);
    g.RenderNumPolygons = 0;
    g.RenderPolygonRAM.fill(nullptr);
    TA::Put(serial, before);
    auto savedExpected = RenderOnce(serial);
    Savestate saved;
    nds->GPU.DoSavestate(&saved);
    saved.Finish();
    assert(!saved.Error);
    DrainStarted(r);
    Equal(TA::Get(r), savedExpected.State);

    // Load while a fresh banded frame is pending, so PreSavestate must join and
    // materialize it before replacing render state. Saved buffers are not serialized.
    Apply(g, rng, scene);
    const auto beforePending = TA::Get(r);
    TA::Put(serial, beforePending);
    const auto completedPending = RenderOnce(serial);
    r.RenderFrame();
    Savestate loaded(saved.Buffer(), saved.Length(), false);
    nds->GPU.DoSavestate(&loaded);
    assert(!loaded.Error);
    DrainStarted(r);
    TA::Put(serial, completedPending.State);
    const auto loadExpected = RenderOnce(serial);
    Equal(TA::Get(r), loadExpected.State);
    assert(TA::LeftoverTokens(r) == 0);
    return Failures ? 1 : 0;
}
