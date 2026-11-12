// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Differential regression harness for the software renderer.
//
// It drives melonDS::GPU scanline by scanline without running the emulated CPUs, mutating
// registers, palette, OAM, VRAM, mappings and renderer lifecycle at scripted points, and
// prints a hash of every finished frame and of every GPU savestate it takes. The same
// source is built against the pristine engine and against the batch2d engine. Their
// output must be identical for every scenario, thread setting and batch size.
//
// With BATCH2D_CANDIDATE defined it also checks prototype internals directly.

#include <chrono>
#include <inttypes.h>
#include <cstddef>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "NDS.h"
#include "GPU.h"
#include "GPU_Soft.h"
#include "Savestate.h"

using namespace melonDS;

// Every heap allocation starts with the same byte pattern. Members the engine never
// initializes would otherwise show whatever earlier allocations left behind, and that
// history differs between the reference and the candidate build.
constexpr unsigned char HeapFill = 0xA5;

void* operator new(size_t size)
{
    void* p = malloc(size ? size : 1);
    if (!p)
        throw std::bad_alloc();
    memset(p, HeapFill, size);
    return p;
}

void* operator new[](size_t size) { return operator new(size); }

void* operator new(size_t size, std::align_val_t align)
{
    size_t a = (size_t)align;
    void* p = aligned_alloc(a, ((size ? size : 1) + a - 1) & ~(a - 1));
    if (!p)
        throw std::bad_alloc();
    memset(p, HeapFill, size);
    return p;
}

void* operator new[](size_t size, std::align_val_t align) { return operator new(size, align); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void operator delete(void* p, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { free(p); }

#ifdef BATCH2D_CANDIDATE
namespace melonDS
{
struct SoftRendererTestAccess
{
    static const SoftBatch2DStats& Stats(const SoftRenderer& r) { return r.Stats; }
    static bool Idle(const SoftRenderer& r)
    {
        return !r.OpenRecord && r.QueueConsumed.load() == r.QueueWrite
            && r.QueuePublished.load() == r.QueueWrite;
    }
    static bool HasWorker(const SoftRenderer& r) { return r.Worker != nullptr; }
    static u32 BatchSize(const SoftRenderer& r) { return r.BatchSize; }
    static u32 RingSize() { return SoftRenderer::RingSize; }
    static const melonDS::GPU2D& Shadow(const SoftRenderer& r, int num)
    {
        return num ? r.Shadow2D_B : r.Shadow2D_A;
    }
};
}
#endif

namespace
{

constexpr u32 MaxLinesPerFrame = 4000;
constexpr u32 StateBufferSize = 16 * 1024 * 1024;

[[noreturn]] void Fail(const char* what)
{
    fflush(stdout);
    fprintf(stderr, "FAIL: %s\n", what);
    fflush(stderr);
    abort();
}

#define CHECK(cond) do { if (!(cond)) Fail(#cond); } while (0)

struct Rng
{
    u64 State;

    explicit Rng(u64 seed) : State(seed)
    {
        // Decorrelate neighbouring seeds: the generator itself only adds a constant per draw.
        State = Next64() ^ (seed << 32);
        State = Next64();
    }

    u64 Next64()
    {
        u64 z = (State += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    u32 Next() { return (u32)(Next64() >> 32); }
    u32 Below(u32 n) { return Next() % n; }
    bool Chance(u32 num, u32 den) { return Below(den) < num; }
};

u64 HashBytes(const void* data, size_t len, u64 hash = 0xCBF29CE484222325ull)
{
    const u8* bytes = (const u8*)data;
    for (size_t i = 0; i < len; i++)
    {
        hash ^= bytes[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

enum Profile
{
    Profile_Raster,    // register, palette and OAM changes only: long queued spans
    Profile_VRAM,      // adds VRAM writes and bank remaps during the frame
    Profile_Capture,   // adds display capture and the VRAM/FIFO display modes
    Profile_Lifecycle, // adds savestates, resets, renderer swaps, thread toggles, VCOUNT writes
    Profile_All,
    Profile_Count,
};

struct Options
{
    bool Threaded = true;
    bool Pace = false;
    u32 Frames = 12;
};

class World
{
public:
    World(u64 seed, int profile, const Options& options)
        : Random(seed), Timing(seed ^ 0xABCDEF), Seed(seed), Kind(profile), Opt(options)
    {
        Nds = std::make_unique<NDS>();
        Nds->Reset();
        // GPU::Reset leaves CaptureEnable alone and GPU.h gives it no initializer; it is
        // only assigned at VCount 0 and 192, so frame 0 would capture on a stale value.
        Gpu().CaptureEnable = false;
        ThreadedNow = Opt.Threaded;
        ApplySettings();
        Setup();
    }

    void Run()
    {
        for (u32 frame = 0; frame < Opt.Frames; frame++)
            RunFrame(frame);

        SaveState("end", Opt.Frames, 0);
        ReportStats();
    }

    NDS& Console() { return *Nds; }
    melonDS::GPU& Gpu() { return Nds->GPU; }

    // A frame as GPU's own event chain would run it, with a hook before and after each HBlank.
    template <typename Hook>
    void RunFrameWith(u32 frame, Hook&& hook)
    {
        melonDS::GPU& gpu = Gpu();
        gpu.StartFrame();
        for (u32 line = 0;; line++)
        {
            if (line >= MaxLinesPerFrame)
                Fail("frame did not end");

            hook(frame, line, 0);
            const bool last = (gpu.VCount == 262) || (gpu.VCount == 511);
            gpu.StartHBlank(line);
            hook(frame, line, 1);
            Pace();
            if (last)
            {
                gpu.FinishFrame(line + 1);
                break;
            }
            gpu.StartScanline(line + 1);
        }

        void* top = nullptr;
        void* bottom = nullptr;
        gpu.GetFramebuffers(&top, &bottom);
        const size_t len = 256 * 192 * sizeof(u32);
        printf("F %" PRIu64 " %u %016" PRIx64 " %016" PRIx64 "\n",
            Seed, frame, HashBytes(top, len), HashBytes(bottom, len));
        CheckIdle();
    }

    void SaveState(const char* tag, u32 frame, u32 line)
    {
        Savestate state(StateBufferSize);
        Gpu().DoSavestate(&state);
        state.Finish();
        CHECK(!state.Error);
        LastState.assign((u8*)state.Buffer(), (u8*)state.Buffer() + state.Length());
        if (const char* dir = getenv("BATCH2D_DUMP"))
        {
            char path[512];
            snprintf(path, sizeof(path), "%s/%" PRIu64 "-%03u.bin", dir, Seed, DumpIndex++);
            FILE* file = fopen(path, "wb");
            CHECK(file);
            fwrite(LastState.data(), 1, LastState.size(), file);
            fclose(file);
        }
        printf("S %" PRIu64 " %s %u %u %016" PRIx64 " w=%02x%02x%02x%02x\n",
            Seed, tag, frame, line, HashBytes(LastState.data(), LastState.size()),
            Gpu().GPU2D_A.Win0Active, Gpu().GPU2D_A.Win1Active,
            Gpu().GPU2D_B.Win0Active, Gpu().GPU2D_B.Win1Active);
    }

    void LoadState()
    {
        if (LastState.empty())
            return;

        Savestate state(LastState.data(), (u32)LastState.size(), false);
        Gpu().DoSavestate(&state);
        CHECK(!state.Error);
    }

    void SetThreaded(bool threaded)
    {
        ThreadedNow = threaded;
        ApplySettings();
    }

    void Write2D(int engine, u32 reg, u16 val)
    {
        melonDS::GPU2D& unit = engine ? Gpu().GPU2D_B : Gpu().GPU2D_A;
        unit.Write16(0x04000000 | (engine ? 0x1000 : 0) | reg, val);
    }

    void SetDispCnt(int engine, u32 val)
    {
        Write2D(engine, 0x000, val & 0xFFFF);
        Write2D(engine, 0x002, val >> 16);
    }

    void SetPower(u16 val)
    {
        Nds->PowerControl9 = val;
        Gpu().SetPowerCnt(val);
    }

    void FillVRAM()
    {
        melonDS::GPU& gpu = Gpu();
        for (int bank = 0; bank < 9; bank++)
        {
            u32 len = gpu.VRAMMask[bank] + 1;
            for (u32 i = 0; i < len; i += 8)
            {
                u64 value = Random.Next64();
                memcpy(&gpu.VRAM[bank][i], &value, 8);
            }
        }
    }

    void FillPaletteAndOAM()
    {
        melonDS::GPU& gpu = Gpu();
        for (u32 i = 0; i < 0x800; i += 2)
        {
            gpu.WritePalette<u16>(i, Random.Next() & 0xFFFF);
            gpu.WriteOAM<u16>(i, RandomOAMWord(i));
        }
    }

    void MapDefaultVRAM()
    {
        melonDS::GPU& gpu = Gpu();
        gpu.MapVRAM_AB(0, 0x81); // A: engine A BG
        gpu.MapVRAM_AB(1, 0x82); // B: engine A OBJ
        gpu.MapVRAM_CD(2, 0x84); // C: engine B BG
        gpu.MapVRAM_CD(3, 0x84); // D: engine B OBJ
        gpu.MapVRAM_E(4, 0x84);  // E: engine A BG extended palettes
        gpu.MapVRAM_FG(5, 0x85); // F: engine A OBJ extended palette
        gpu.MapVRAM_FG(6, 0x83); // G: texture palette
        gpu.MapVRAM_H(7, 0x82);  // H: engine B BG extended palettes
        gpu.MapVRAM_I(8, 0x83);  // I: engine B OBJ extended palette
    }

    Rng Random;

private:
    void ApplySettings()
    {
        RendererSettings settings {};
        settings.ScaleFactor = 1;
        settings.Threaded = ThreadedNow;
        Nds->GetRenderer().SetRenderSettings(settings);
    }

    void Setup()
    {
        melonDS::GPU& gpu = Gpu();
        SetPower(0x820F);
        FillVRAM();
        MapDefaultVRAM();
        FillPaletteAndOAM();

        for (int engine = 0; engine < 2; engine++)
        {
            for (u32 reg = 0x008; reg <= 0x054; reg += 2)
                Write2D(engine, reg, Random.Next() & 0xFFFF);
            SetDispCnt(engine, RandomDispCnt(engine));
        }

        gpu.Write16(0x0400006C, 0);
        gpu.Write16(0x0400106C, 0);
        gpu.Write32(0x04000064, 0);
        for (int i = 0; i < 256; i++)
            gpu.DispFIFOBuffer[i] = Random.Next() & 0xFFFF;

        // 3D: no polygons, a visible clear colour, and no fog. With fog a 3D frame that is
        // re-rendered mid-frame (savestate, reset) races the 2D reads even in the pristine engine.
        gpu.GPU3D.Write32(0x04000060, Random.Next() & 0x4000);
        gpu.GPU3D.Write32(0x04000350, (Random.Next() & 0x00007FFF) | 0x001F0000 | ((Random.Next() & 0x3F) << 24));
    }

    u16 RandomOAMWord(u32 addr)
    {
        u16 val = Random.Next() & 0xFFFF;
        // Keep a fair share of sprites enabled and on screen.
        if ((addr & 0x6) == 0 && Random.Chance(1, 2))
            val = (val & 0xFC00) | Random.Below(192) | (Random.Below(2) << 8);
        return val;
    }

    u32 RandomDispCnt(int engine)
    {
        u32 val = Random.Next();
        // Forced blank is rare, layers are mostly on, the display is mostly in normal mode.
        if (!Random.Chance(1, 16))
            val &= ~0x80u;
        if (!Random.Chance(1, 4))
            val |= 0x1F00;

        // See Hide3D: the 3D layer stays off BG0 where the pristine engine itself would race.
        if (Hide3D() && engine == 0)
            val &= ~0x8u;

        u32 mode = 1;
        const bool liveModes = (Kind == Profile_Capture) || (Kind == Profile_All);
        if (Random.Chance(1, 20))
            mode = 0;
        else if (liveModes && engine == 0 && Random.Chance(1, 6))
            mode = 2 + Random.Below(2);
        val = (val & ~0x00030000u) | (mode << 16);
        return val;
    }

    void RasterOp()
    {
        melonDS::GPU& gpu = Gpu();
        switch (Random.Below(12))
        {
        case 0: case 1: case 2: // palette, the common mid-frame write
            gpu.WritePalette<u16>(Random.Below(0x400) * 2, Random.Next() & 0xFFFF);
            break;
        case 3: // palette write that changes nothing
            {
                u32 addr = Random.Below(0x400) * 2;
                gpu.WritePalette<u16>(addr, gpu.ReadPalette<u16>(addr));
            }
            break;
        case 4: case 5:
            {
                u32 addr = Random.Below(0x400) * 2;
                gpu.WriteOAM<u16>(addr, RandomOAMWord(addr));
            }
            break;
        case 6: case 7: case 8:
            {
                // scroll, affine, window, mosaic and blend registers
                u32 reg = 0x008 + Random.Below(39) * 2;
                Write2D(Random.Below(2), reg, Random.Next() & 0xFFFF);
            }
            break;
        case 9:
            {
                int engine = Random.Below(2);
                SetDispCnt(engine, RandomDispCnt(engine));
            }
            break;
        case 10:
            gpu.Write16(Random.Below(2) ? 0x0400106C : 0x0400006C,
                Random.Chance(1, 2) ? 0 : (Random.Next() & 0xC01F));
            break;
        case 11:
            if (Random.Chance(1, 4))
                SetPower(0x000F | (Random.Next() & 0x8200) | (Random.Chance(7, 8) ? 0x0200 : 0));
            else
                gpu.GPU3D.Write32(0x04000350,
                    (Random.Next() & 0x00007FFF) | 0x001F0000 | ((Random.Next() & 0x3F) << 24));
            break;
        }
    }

    void VRAMOp()
    {
        melonDS::GPU& gpu = Gpu();
        u32 count = 1 + Random.Below(24);
        u32 base = Random.Next();
        switch (Random.Below(7))
        {
        case 0:
            for (u32 i = 0; i < count; i++)
                gpu.WriteVRAM_ABG<u16>(0x06000000 + ((base + i * 2) & 0x7FFFE), Random.Next());
            break;
        case 1:
            for (u32 i = 0; i < count; i++)
                gpu.WriteVRAM_BBG<u16>(0x06200000 + ((base + i * 2) & 0x1FFFE), Random.Next());
            break;
        case 2:
            for (u32 i = 0; i < count; i++)
                gpu.WriteVRAM_AOBJ<u16>(0x06400000 + ((base + i * 2) & 0x3FFFE), Random.Next());
            break;
        case 3:
            for (u32 i = 0; i < count; i++)
                gpu.WriteVRAM_BOBJ<u16>(0x06600000 + ((base + i * 2) & 0x1FFFE), Random.Next());
            break;
        case 4:
            for (u32 i = 0; i < count; i++)
                gpu.WriteVRAM_LCDC<u16>(0x06800000 + ((base + i * 2) & 0xFFFFE), Random.Next());
            break;
        case 5:
            RemapOp();
            break;
        case 6:
            MapDefaultVRAM();
            break;
        }
    }

    void RemapOp()
    {
        melonDS::GPU& gpu = Gpu();
        u8 cnt = (Random.Chance(7, 8) ? 0x80 : 0) | (Random.Next() & 0x1F);
        switch (Random.Below(9))
        {
        case 0: gpu.MapVRAM_AB(0, cnt); break;
        case 1: gpu.MapVRAM_AB(1, cnt); break;
        case 2: gpu.MapVRAM_CD(2, cnt); break;
        case 3: gpu.MapVRAM_CD(3, cnt); break;
        case 4: gpu.MapVRAM_E(4, cnt); break;
        case 5: gpu.MapVRAM_FG(5, cnt); break;
        case 6: gpu.MapVRAM_FG(6, cnt); break;
        case 7: gpu.MapVRAM_H(7, cnt); break;
        case 8: gpu.MapVRAM_I(8, cnt); break;
        }
    }

    void CaptureOp()
    {
        melonDS::GPU& gpu = Gpu();
        switch (Random.Below(4))
        {
        case 0:
            {
                // Request a capture for the next frame into a bank that is mapped for it.
                u32 bank = Random.Below(4);
                if (bank < 2) gpu.MapVRAM_AB(bank, 0x80);
                else          gpu.MapVRAM_CD(bank, 0x80);
                u32 cnt = 0x80000000 | (Random.Next() & 0x6F3F1F1F & ~0x00030000u) | (bank << 16);
                if (Hide3D())
                    cnt &= ~(1u << 24); // capture the 2D output, not the 3D layer
                gpu.Write32(0x04000064, cnt);
            }
            break;
        case 1:
            for (int i = 0; i < 256; i++)
                gpu.DispFIFOBuffer[i] = Random.Next() & 0xFFFF;
            break;
        case 2:
            SetDispCnt(0, RandomDispCnt(0));
            break;
        case 3:
            MapDefaultVRAM();
            break;
        }
    }

    void LifecycleOp(u32 frame, u32 line)
    {
        melonDS::GPU& gpu = Gpu();
        switch (Random.Below(12))
        {
        case 0: case 1: case 2:
            SaveState("mid", frame, line);
            break;
        case 3: case 4:
            SaveState("pre", frame, line);
            LoadState();
            break;
        case 5:
            LoadState();
            break;
        case 6:
            SetThreaded(!ThreadedNow);
            break;
        case 7:
            Nds->SetRenderer(std::make_unique<SoftRenderer>(*Nds));
            ApplySettings();
            break;
        case 8:
            gpu.Restart3DFrame();
            break;
        case 9:
            // A write to VCOUNT near its current value, as timing tricks do.
            gpu.SetVCount((gpu.VCount + Random.Below(5)) & 0x1FF, 0xFFFF);
            break;
        case 10:
            if (Random.Chance(1, 3))
            {
                gpu.Reset();
                Setup();
            }
            break;
        case 11:
            if (Random.Chance(1, 4))
                gpu.Stop();
            break;
        }
    }

    void Hook(u32 frame, u32 line, int phase)
    {
        // Most lines see no write at all, some see a burst.
        if (!Random.Chance(1, 3))
            return;

        u32 count = 1 + Random.Below(Random.Chance(1, 8) ? 12 : 3);
        for (u32 i = 0; i < count; i++)
        {
            u32 pick = Random.Below(100);
            const bool vram = (Kind == Profile_VRAM) || (Kind == Profile_All);
            const bool capture = (Kind == Profile_Capture) || (Kind == Profile_All);
            const bool lifecycle = (Kind == Profile_Lifecycle) || (Kind == Profile_All);

            if (vram && pick < 6)
                VRAMOp();
            else if (capture && pick >= 6 && pick < 9)
                CaptureOp();
            else if (lifecycle && pick >= 9 && pick < 11)
                LifecycleOp(frame, line);
            else
                RasterOp();
        }
        (void)phase;
    }

    // With a threaded 3D renderer, restarting its frame after scanlines have been read
    // (savestate, reset, wake from sleep, thread toggle) leaves scanline tokens behind in
    // the pristine engine too, so later frames may read 3D rows before they are redrawn.
    // What such a frame shows depends on thread timing and cannot be compared, so profiles
    // that do this mid-frame keep the 3D layer out of the 2D output. The 3D thread still
    // runs and its semaphores are still consumed line by line.
    bool Hide3D() const
    {
        return (Kind == Profile_Lifecycle) || (Kind == Profile_All);
    }

    void RunFrame(u32 frame)
    {
        RunFrameWith(frame, [this](u32 f, u32 line, int phase) { Hook(f, line, phase); });
    }

    void Pace()
    {
        // Timing only: lets the worker catch up or fall behind at varying points.
        if (!Opt.Pace)
            return;

        u32 pick = Timing.Below(64);
        if (pick < 4)
            std::this_thread::sleep_for(std::chrono::microseconds(20 + Timing.Below(200)));
        else if (pick < 16)
            std::this_thread::yield();
    }

    void CheckIdle()
    {
#ifdef BATCH2D_CANDIDATE
        auto* renderer = dynamic_cast<SoftRenderer*>(&Nds->GetRenderer());
        CHECK(renderer);
        // SwapBuffers drains, so nothing may be queued or open once a frame has finished.
        CHECK(SoftRendererTestAccess::Idle(*renderer));
#endif
    }

    void ReportStats()
    {
#ifdef BATCH2D_CANDIDATE
        auto* renderer = dynamic_cast<SoftRenderer*>(&Nds->GetRenderer());
        const SoftBatch2DStats& stats = SoftRendererTestAccess::Stats(*renderer);
        fprintf(stderr, "stats seed=%" PRIu64 " profile=%d queued=%" PRIu64 " inline=%" PRIu64
            " inline_sprites=%" PRIu64 " publishes=%" PRIu64 " fences=%" PRIu64
            " dirty_waits=%" PRIu64 " full_waits=%" PRIu64 "\n",
            Seed, Kind, stats.QueuedLines, stats.InlineLines, stats.InlineSprites,
            stats.Publishes, stats.Fences, stats.DirtyWaits, stats.FullWaits);
#endif
    }

    std::unique_ptr<NDS> Nds;
    Rng Timing;
    u64 Seed;
    int Kind;
    Options Opt;
    bool ThreadedNow = true;
    std::vector<u8> LastState;
    u32 DumpIndex = 0;
};

#ifdef BATCH2D_CANDIDATE
using SoftBatchStatsSnapshot = SoftBatch2DStats;

SoftBatch2DStats SnapshotStats(World& world)
{
    auto* renderer = dynamic_cast<SoftRenderer*>(&world.Console().GetRenderer());
    CHECK(renderer);
    return SoftRendererTestAccess::Stats(*renderer);
}

SoftBatch2DStats StatsSince(World& world, const SoftBatch2DStats& base)
{
    SoftBatch2DStats now = SnapshotStats(world);
    now.QueuedLines -= base.QueuedLines;
    now.InlineLines -= base.InlineLines;
    now.InlineSprites -= base.InlineSprites;
    now.Publishes -= base.Publishes;
    now.Fences -= base.Fences;
    now.DirtyWaits -= base.DirtyWaits;
    now.FullWaits -= base.FullWaits;
    return now;
}
#else
struct SoftBatchStatsSnapshot {};
SoftBatchStatsSnapshot SnapshotStats(World&) { return {}; }
#endif

// A plain text-mode scene on both engines with nothing changing during the frame.
void SetupPlainScene(World& world)
{
    world.SetPower(0x8203);
    world.FillVRAM();
    world.MapDefaultVRAM();
    world.FillPaletteAndOAM();
    for (int engine = 0; engine < 2; engine++)
    {
        for (u32 reg = 0x008; reg <= 0x054; reg += 2)
            world.Write2D(engine, reg, 0);
        world.Write2D(engine, 0x008, 0x0000);
        world.Write2D(engine, 0x00A, 0x0101);
        world.SetDispCnt(engine, 0x00011300); // mode 0, BG0, BG1 and OBJ on, normal display
    }
}

// Window 0 with x1 > x2 stays open across the right edge, so bit 1 of Win0Active is set
// at the end of each line and must carry into the next one and into savestates.
void ScenarioCarry(const Options& options)
{
    World world(1001, Profile_Raster, options);
    SetupPlainScene(world);
    for (int engine = 0; engine < 2; engine++)
    {
        world.SetDispCnt(engine, 0x00013300 | (engine ? 0x4000 : 0)); // window 0, plus window 1 on B
        world.Write2D(engine, 0x040, (200 << 8) | 40);  // WIN0H: x1=200, x2=40
        world.Write2D(engine, 0x044, (10 << 8) | 150);  // WIN0V: y1=10, y2=150
        world.Write2D(engine, 0x042, (16 << 8) | 16);   // WIN1H: x1 == x2 only ever closes
        world.Write2D(engine, 0x046, (0 << 8) | 191);
        world.Write2D(engine, 0x048, 0x1311);           // WININ
        world.Write2D(engine, 0x04A, 0x0002);           // WINOUT
    }

    for (u32 frame = 0; frame < 4; frame++)
    {
        world.RunFrameWith(frame, [&world](u32 f, u32 line, int phase)
        {
            if (phase != 1)
                return;

            // After HBlank of these lines every earlier line has been emulated.
            if (line == 5 || line == 60 || line == 149 || line == 150 || line == 191 || line == 230)
                world.SaveState("carry", f, line);
            if (line == 60)
            {
                // Bit 0 from UpdateWindows, bit 1 carried out of the lines drawn so far.
                CHECK(world.Gpu().GPU2D_A.Win0Active == 0x3);
                CHECK(world.Gpu().GPU2D_B.Win0Active == 0x3);
            }
            if (line == 100)
                world.Write2D(0, 0x040, (30 << 8) | 220); // x1 < x2: closes before the edge
            if (line == 120)
                world.Write2D(0, 0x040, (200 << 8) | 40);
            if (line == 130 && (f & 1))
            {
                // Round trip through a savestate in the middle of the window.
                world.SaveState("carry-load", f, line);
                world.LoadState();
            }
        });
    }
}

// Palette, scroll and OAM rewritten on every line, with no VRAM writes: the pattern the
// batching is meant for. Each line must be drawn with exactly the values of its own HBlank.
void ScenarioRaster(const Options& options)
{
    World world(1002, Profile_Raster, options);
    SetupPlainScene(world);
    // Frame 0 warms the VRAM caches; the counters are checked over frames 1 to 4.
    SoftBatchStatsSnapshot warm;
    for (u32 frame = 0; frame < 5; frame++)
    {
        if (frame == 1)
            warm = SnapshotStats(world);
        world.RunFrameWith(frame, [&world](u32 f, u32 line, int phase)
        {
            if (phase != 0)
                return;

            melonDS::GPU& gpu = world.Gpu();
            gpu.WritePalette<u16>(0, (line * 37 + f) & 0x7FFF);          // backdrop A
            gpu.WritePalette<u16>(0x400, (line * 91 + f * 3) & 0x7FFF);  // backdrop B
            for (u32 i = 0; i < 8; i++)
                gpu.WritePalette<u16>(world.Random.Below(0x400) * 2, world.Random.Next() & 0xFFFF);
            world.Write2D(0, 0x010, line * 3);
            world.Write2D(1, 0x012, line * 5);
            gpu.WriteOAM<u16>(world.Random.Below(0x100) * 8, world.Random.Below(192));
            gpu.WriteOAM<u16>(world.Random.Below(0x100) * 8 + 2, world.Random.Next() & 0x01FF);
        });
    }

#ifdef BATCH2D_CANDIDATE
    auto* renderer = dynamic_cast<SoftRenderer*>(&world.Console().GetRenderer());
    const SoftBatch2DStats stats = StatsSince(world, warm);
    const u32 batch = SoftRendererTestAccess::BatchSize(*renderer);
    if (SoftRendererTestAccess::HasWorker(*renderer))
    {
        // Every visible line is handed to the worker and none is drawn on this thread.
        CHECK(stats.QueuedLines == 4 * 192);
        CHECK(stats.InlineLines == 0);
        // The sprite prologue for line 0 is the only DrawSprites drawn here.
        CHECK(stats.InlineSprites == 4);
        CHECK(stats.DirtyWaits == 0);
        // The worker is woken per batch, never per line. Waiting on a full ring or a drain
        // publishes a partial batch early, so those add at most one wake each.
        const u64 batches = 4 * ((192 + batch - 1) / batch);
        CHECK(stats.Publishes >= batches);
        CHECK(stats.Publishes <= batches + stats.Fences);
    }
    else
    {
        CHECK(stats.QueuedLines == 0);
        CHECK(stats.Publishes == 0);
        CHECK(stats.Fences == 0);
        CHECK(stats.InlineLines == 4 * 192);
    }
#endif
}

// VRAM written while lines are still queued: the caches may only change once those lines
// are drawn, and after a few such waits the rest of the frame is drawn immediately.
void ScenarioDirty(const Options& options)
{
    World world(1003, Profile_VRAM, options);
    SetupPlainScene(world);
    for (u32 frame = 0; frame < 4; frame++)
    {
        world.RunFrameWith(frame, [&world](u32 f, u32 line, int phase)
        {
            melonDS::GPU& gpu = world.Gpu();
            // Frames 0 and 2: a few writes spread over the frame. Frames 1 and 3: every line.
            const bool every = (f & 1) != 0;
            if (phase == 0 && line < 192 && (every || (line % 48) == 20))
            {
                gpu.WriteVRAM_ABG<u16>(0x06000000 + ((line * 64) & 0xFFFE), world.Random.Next());
                gpu.WriteVRAM_BOBJ<u16>(0x06600000 + ((line * 32) & 0xFFFE), world.Random.Next());
            }
            if (phase == 1 && line == 96)
                gpu.MapVRAM_E(4, (f & 2) ? 0x84 : 0x00);
        });
    }

#ifdef BATCH2D_CANDIDATE
    auto* renderer = dynamic_cast<SoftRenderer*>(&world.Console().GetRenderer());
    const SoftBatch2DStats& stats = SoftRendererTestAccess::Stats(*renderer);
    if (SoftRendererTestAccess::HasWorker(*renderer))
    {
        CHECK(stats.QueuedLines > 0);
        CHECK(stats.DirtyWaits > 0);
        // Every frame here exceeds the wait budget at some point and finishes on this thread;
        // the two every-line frames do so within their first few lines.
        CHECK(stats.InlineLines >= 2 * 150);
        CHECK(stats.QueuedLines + stats.InlineLines == 4 * 192);
    }
#endif
}

// Display capture and the VRAM/FIFO display modes read and write live memory, so such
// lines are drawn immediately, and may sit between queued lines of the same frame.
void ScenarioCapture(const Options& options)
{
    World world(1004, Profile_Capture, options);
    SetupPlainScene(world);
    melonDS::GPU& gpu = world.Gpu();
    gpu.MapVRAM_CD(3, 0x80); // D: LCDC, capture destination
    for (u32 frame = 0; frame < 6; frame++)
    {
        if (frame == 1 || frame == 4)
            gpu.Write32(0x04000064, 0x80000000 | (3 << 16) | (3 << 20) | (2 << 29) | 0x0810);
        world.RunFrameWith(frame, [&world](u32 f, u32 line, int phase)
        {
            if (phase != 0)
                return;

            // Frame 2 shows the captured bank; frames 3 and 5 switch modes during the frame.
            if (f == 2 && line == 0)
                world.SetDispCnt(0, 0x00021300 | (3 << 18));
            if (f == 3 && line == 0)
                world.SetDispCnt(0, 0x00011300);
            if (f == 3 && line == 64)
                world.SetDispCnt(0, 0x00031300);
            if ((f == 3 || f == 5) && line == 128)
                world.SetDispCnt(0, 0x00011300);
            if (f == 5 && line == 40)
                world.SetDispCnt(0, 0x00021300 | (3 << 18));
            if (line == 150)
                world.SaveState("capture", f, line);
        });
    }

#ifdef BATCH2D_CANDIDATE
    auto* renderer = dynamic_cast<SoftRenderer*>(&world.Console().GetRenderer());
    const SoftBatch2DStats& stats = SoftRendererTestAccess::Stats(*renderer);
    if (SoftRendererTestAccess::HasWorker(*renderer))
    {
        // Frames 1, 2 and 4 are entirely immediate, 3 and 5 are mixed, 0 is entirely queued.
        CHECK(stats.InlineLines == 3 * 192 + 64 + 88);
        CHECK(stats.QueuedLines == 192 + 128 + 104);
    }
#endif
}

// Lifecycle calls placed while lines are queued, including the ones GPU makes itself.
void ScenarioLifecycle(const Options& options)
{
    World world(1005, Profile_Lifecycle, options);
    SetupPlainScene(world);
    for (u32 frame = 0; frame < 10; frame++)
    {
        world.RunFrameWith(frame, [&world](u32 f, u32 line, int phase)
        {
            if (phase != 1 || line != 77)
                return;

            melonDS::GPU& gpu = world.Gpu();
            switch (f)
            {
            case 0: world.SaveState("life", f, line); break;
            case 1: world.SaveState("life", f, line); world.LoadState(); break;
            case 2: gpu.Restart3DFrame(); break;
            case 3: world.SetThreaded(false); break;
            case 4: world.SetThreaded(true); break;
            case 5:
                {
                    world.Console().SetRenderer(std::make_unique<SoftRenderer>(world.Console()));
                    world.SetThreaded(true);
                }
                break;
            case 6: gpu.SetVCount(gpu.VCount + 2, 0xFFFF); break;
            case 7: gpu.Stop(); break;
            case 8: gpu.Reset(); SetupPlainScene(world); break;
            case 9: world.LoadState(); break;
            }
        });
    }
}

// The 3D layer on screen, scrolled per line and recoloured per frame, with lifecycle calls
// between frames, where a frontend makes them and where the 3D thread's scanline
// accounting is exact. A VCOUNT write (frame abort) is the one mid-frame case included.
void ScenarioBoundary(const Options& options)
{
    World world(1006, Profile_Raster, options);
    auto setup = [&world]()
    {
        SetupPlainScene(world);
        world.SetPower(0x820F);
        world.SetDispCnt(0, 0x00011308); // BG0 shows the 3D layer
        world.Gpu().GPU3D.Write32(0x04000060, 0);
        world.Gpu().GPU3D.Write32(0x04000350, 0x1F1F7C1F);
    };
    setup();

    for (u32 frame = 0; frame < 14; frame++)
    {
        melonDS::GPU& gpu = world.Gpu();
        switch (frame)
        {
        case 2: world.SaveState("edge", frame, 0); break;
        case 3: world.SaveState("edge", frame, 0); world.LoadState(); break;
        case 4: world.SetThreaded(false); break;
        case 6: world.SetThreaded(true); break;
        case 7:
            world.Console().SetRenderer(std::make_unique<SoftRenderer>(world.Console()));
            world.SetThreaded(true);
            break;
        case 9: gpu.Reset(); setup(); break;
        case 11: gpu.Stop(); break;
        case 12: world.LoadState(); break;
        }

        world.RunFrameWith(frame, [&world](u32 f, u32 line, int phase)
        {
            if (phase != 0)
                return;

            melonDS::GPU& gpu = world.Gpu();
            world.Write2D(0, 0x010, (line * 2 + f) & 0x1FF); // BG0HOFS scrolls the 3D layer
            gpu.WritePalette<u16>(0, (line * 53 + f) & 0x7FFF);
            if (line == 100)
                gpu.GPU3D.Write32(0x04000350, 0x1F1F0000 | ((f * 0x0421) & 0x7FFF));
            if (f == 8 && line == 77)
                gpu.SetVCount(gpu.VCount + 2, 0xFFFF);
        });
    }
}

#ifdef BATCH2D_CANDIDATE
// Capture and apply must carry every GPU2D data member, and must treat the window carry
// bit as the renderer's own.
void UnitCaptureApply()
{
    auto nds = std::make_unique<NDS>();
    nds->Reset();
    melonDS::GPU2D& live = nds->GPU.GPU2D_A;
    melonDS::GPU2D shadow(0, nds->GPU);
    shadow.Reset();

    Rng random(77);
#define FILL(field) \
    { u8* bytes = (u8*)&live.field; for (size_t i = 0; i < sizeof(live.field); i++) bytes[i] = random.Next(); }
    SOFT2D_STATE_FIELDS(FILL)
#undef FILL
    live.BGMosaicLatch = true;
    live.OBJMosaicLatch = false;
    live.Enabled = true;
    live.Win0Active = 0x01; // live carries a stale bit 1 of zero
    live.Win1Active = 0x02; // live carries a stale bit 1 of one
    shadow.Win0Active = 0x02;
    shadow.Win1Active = 0x00;

    Soft2DState state;
    SoftRenderer2D::CaptureState(live, state);
    SoftRenderer2D::ApplyState(state, shadow);

#define SAME(field) \
    if (strcmp(#field, "Win0Active") && strcmp(#field, "Win1Active")) \
        CHECK(!memcmp(&shadow.field, &live.field, sizeof(live.field)));
    SOFT2D_STATE_FIELDS(SAME)
#undef SAME
    CHECK(shadow.Win0Active == 0x03); // bit 0 from the capture, bit 1 kept
    CHECK(shadow.Win1Active == 0x00);
    CHECK(shadow.Num == 0);

    // The field list must cover GPU2D's data members with no gap: each listed member starts
    // where the previous one ends, allowing only for alignment.
    size_t end = offsetof(melonDS::GPU2D, Num) + sizeof(live.Num);
#define LAYOUT(field) \
    { \
        size_t align = alignof(decltype(live.field)); \
        size_t expected = (end + align - 1) & ~(align - 1); \
        CHECK(offsetof(melonDS::GPU2D, field) == expected); \
        end = expected + sizeof(live.field); \
    }
    SOFT2D_STATE_FIELDS(LAYOUT)
#undef LAYOUT
    // Only the reference to GPU may follow the last listed member.
    CHECK(((end + alignof(void*) - 1) & ~(alignof(void*) - 1)) + sizeof(void*) == sizeof(melonDS::GPU2D));

    CHECK(SoftRendererTestAccess::RingSize() == 32);
}
#endif

void RunNamed(const std::string& name, const Options& options)
{
    if (name == "carry") ScenarioCarry(options);
    else if (name == "raster") ScenarioRaster(options);
    else if (name == "dirty") ScenarioDirty(options);
    else if (name == "capture") ScenarioCapture(options);
    else if (name == "lifecycle") ScenarioLifecycle(options);
    else if (name == "boundary") ScenarioBoundary(options);
    else if (name == "unit")
    {
#ifdef BATCH2D_CANDIDATE
        UnitCaptureApply();
#endif
        printf("U ok\n");
    }
    else Fail("unknown scenario");
}

}

int main(int argc, char** argv)
{
    Options options;
    std::vector<std::string> names;
    u64 seedFirst = 0, seedCount = 0;

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--unthreaded") options.Threaded = false;
        else if (arg == "--pace") options.Pace = true;
        else if (arg == "--frames" && i + 1 < argc) options.Frames = strtoul(argv[++i], nullptr, 10);
        else if (arg == "--seeds" && i + 2 < argc)
        {
            seedFirst = strtoull(argv[++i], nullptr, 10);
            seedCount = strtoull(argv[++i], nullptr, 10);
        }
        else names.push_back(arg);
    }

    for (const std::string& name : names)
        RunNamed(name, options);

    for (u64 seed = seedFirst; seed < seedFirst + seedCount; seed++)
    {
        World world(seed, (int)(seed % Profile_Count), options);
        world.Run();
    }

    fflush(stdout);
    return 0;
}
