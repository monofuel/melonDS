/*
    Copyright 2016-2026 melonDS team

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


#include <stdlib.h>

#include "NDS.h"
#include "GPU_Soft.h"
#include "GPU_ColorOp.h"

namespace melonDS
{

// Coarse 2D batching.
//
// The emulation thread still runs every register latch and every VRAM cache check at its
// original point in the frame. What moves to a worker thread is the pixel work: for each
// HBlank the emulation thread captures a SoftLineRecord and the worker draws it later.
//
// What the worker may touch: published records, the shadow GPU2D objects, the 2D
// renderers' line buffers, Output2D/Output3D, the back framebuffer rows named by the
// records, the VRAMFlat_* caches (read only), and the 3D renderer's finished scanlines
// through SoftRenderer3D::GetLine. It never reads live registers, palette, OAM, raw VRAM,
// VRAM mappings or dirty bits.
//
// Ownership passes to the worker when QueuePublished moves past a record and returns when
// QueueConsumed moves past it. The emulation thread waits for that (Drain/WaitIdle) before
// it changes anything on the list above, and before it draws a line itself.

static u32 ReadBatchSize(u32 fallback, u32 max)
{
    // Private validation control: 0 draws every line on the emulation thread,
    // 1..max sets how many lines are handed over per wake of the worker.
    const char* text = getenv("MONOMELON_BATCH2D");
    if (!text || !*text)
        return fallback;

    char* end = nullptr;
    unsigned long value = strtoul(text, &end, 10);
    if (*end || value > max)
        return fallback;

    return (u32)value;
}

SoftRenderer::SoftRenderer(melonDS::NDS& nds)
    : Renderer(nds.GPU), Shadow2D_A(0, nds.GPU), Shadow2D_B(1, nds.GPU)
{
    const size_t len = 256 * 192;
    Framebuffer[0][0] = new u32[len];
    Framebuffer[0][1] = new u32[len];
    Framebuffer[1][0] = new u32[len];
    Framebuffer[1][1] = new u32[len];
    BackBuffer = 0;

    Shadow2D_A.Reset();
    Shadow2D_B.Reset();

    auto rend2dA = std::make_unique<SoftRenderer2D>(Shadow2D_A, *this);
    auto rend2dB = std::make_unique<SoftRenderer2D>(Shadow2D_B, *this);
    auto rend3d = std::make_unique<SoftRenderer3D>(GPU.GPU3D, *this);
    Soft2D[0] = rend2dA.get();
    Soft2D[1] = rend2dB.get();
    Soft3D = rend3d.get();
    Rend2D_A = std::move(rend2dA);
    Rend2D_B = std::move(rend2dB);
    Rend3D = std::move(rend3d);

    Sema_Work = Platform::Semaphore_Create();
    Sema_Fence = Platform::Semaphore_Create();
    BatchSize = ReadBatchSize(DefaultBatchSize, MaxBatchSize);
}

SoftRenderer::~SoftRenderer()
{
    StopWorker();
    MergeWindowCarry();

    Platform::Semaphore_Free(Sema_Work);
    Platform::Semaphore_Free(Sema_Fence);

    delete[] Framebuffer[0][0];
    delete[] Framebuffer[0][1];
    delete[] Framebuffer[1][0];
    delete[] Framebuffer[1][1];
}

void SoftRenderer::Reset()
{
    // GPU::Reset clears the live 2D state before it gets here. Queued lines are finished,
    // but their window carry is dropped in favor of the freshly reset live value.
    WaitIdle();

    const size_t len = 256 * 192 * sizeof(u32);
    memset(Framebuffer[0][0], 0, len);
    memset(Framebuffer[0][1], 0, len);
    memset(Framebuffer[1][0], 0, len);
    memset(Framebuffer[1][1], 0, len);

    Rend2D_A->Reset();
    Rend2D_B->Reset();
    Rend3D->Reset();

    ReloadWindowCarry();
    DirtyWaitsThisFrame = 0;
}

void SoftRenderer::Stop()
{
    Drain();

    // clear framebuffers to black
    const size_t len = 256 * 192 * sizeof(u32);
    memset(Framebuffer[0][0], 0, len);
    memset(Framebuffer[0][1], 0, len);
    memset(Framebuffer[1][0], 0, len);
    memset(Framebuffer[1][1], 0, len);
}


void SoftRenderer::PreSavestate()
{
    // The savestate must see the window carry of every line emulated so far, and
    // SetupRenderThread resets the scanline semaphore the queued lines consume.
    Drain();

    if (Soft3D->IsThreaded())
        Soft3D->SetupRenderThread();
}

void SoftRenderer::PostSavestate()
{
    if (Soft3D->IsThreaded())
        Soft3D->EnableRenderThread();

    ReloadWindowCarry();
}


void SoftRenderer::SetRenderSettings(RendererSettings& settings)
{
    Drain();

    Soft3D->SetThreaded(settings.Threaded);

    if (settings.Threaded && BatchSize)
        StartWorker();
    else
        StopWorker();
}


void SoftRenderer::StartWorker()
{
    if (Worker)
        return;

    // A stopped worker may have left tokens for publishes it had already drawn.
    Platform::Semaphore_Reset(Sema_Work);
    Worker = Platform::Thread_Create([this]() {
        WorkerFunc();
    });
}

void SoftRenderer::StopWorker()
{
    if (!Worker)
        return;

    WaitIdle();

    Ring[QueueWrite & RingMask].Kind = SoftLineRecord::Kind_Stop;
    QueueWrite++;
    PublishRecords();

    Platform::Thread_Wait(Worker);
    Platform::Thread_Free(Worker);
    Worker = nullptr;
}

void SoftRenderer::WorkerFunc()
{
    u32 pos = QueueConsumed.load(std::memory_order_relaxed);

    for (;;)
    {
        Platform::Semaphore_Wait(Sema_Work);

        // Every publish posts Sema_Work after it stores QueuePublished, so a record that
        // becomes visible after the last load below always has a token left to wake us.
        u32 end = QueuePublished.load(std::memory_order_acquire);
        while (pos != end)
        {
            SoftLineRecord& rec = Ring[pos & RingMask];
            pos++;

            if (rec.Kind == SoftLineRecord::Kind_Line)
            {
                RenderRecord(rec);
                QueueConsumed.store(pos, std::memory_order_release);
            }
            else if (rec.Kind == SoftLineRecord::Kind_Fence)
            {
                QueueConsumed.store(pos, std::memory_order_release);
                Platform::Semaphore_Post(Sema_Fence);
            }
            else
            {
                QueueConsumed.store(pos, std::memory_order_release);
                return;
            }

            if (pos == end)
                end = QueuePublished.load(std::memory_order_acquire);
        }
    }
}

bool SoftRenderer::QueueBusy() const
{
    return OpenRecord || QueueConsumed.load(std::memory_order_acquire) != QueueWrite;
}

SoftLineRecord& SoftRenderer::AcquireRecord()
{
    // A slot is reused only after the worker has moved past it.
    if (QueueWrite - QueueConsumed.load(std::memory_order_acquire) >= RingSize - 1)
    {
        Stats.FullWaits++;
        WaitIdle();
    }

    return Ring[QueueWrite & RingMask];
}

void SoftRenderer::CommitRecord(bool publish)
{
    OpenRecord = nullptr;
    QueueWrite++;
    Stats.QueuedLines++;

    if (publish || (QueueWrite - QueuePublishedLocal) >= BatchSize)
        PublishRecords();
}

void SoftRenderer::PublishRecords()
{
    if (QueuePublishedLocal == QueueWrite)
        return;

    QueuePublishedLocal = QueueWrite;
    QueuePublished.store(QueueWrite, std::memory_order_release);
    Platform::Semaphore_Post(Sema_Work);
    Stats.Publishes++;
}

void SoftRenderer::WaitIdle()
{
    if (OpenRecord)
        CommitRecord(false);

    if (!Worker)
        return;

    if (QueueConsumed.load(std::memory_order_acquire) == QueueWrite)
        return;

    Ring[QueueWrite & RingMask].Kind = SoftLineRecord::Kind_Fence;
    QueueWrite++;
    PublishRecords();
    Platform::Semaphore_Wait(Sema_Fence);
    Stats.Fences++;
}

void SoftRenderer::Drain()
{
    WaitIdle();
    MergeWindowCarry();
}

void SoftRenderer::MergeWindowCarry()
{
    // Bit 0 is maintained live by GPU2D::UpdateWindows. Bit 1 is whatever the last drawn
    // line left behind, which is what GPU2D::DoSavestate has to store.
    GPU.GPU2D_A.Win0Active = (GPU.GPU2D_A.Win0Active & ~0x2) | (Shadow2D_A.Win0Active & 0x2);
    GPU.GPU2D_A.Win1Active = (GPU.GPU2D_A.Win1Active & ~0x2) | (Shadow2D_A.Win1Active & 0x2);
    GPU.GPU2D_B.Win0Active = (GPU.GPU2D_B.Win0Active & ~0x2) | (Shadow2D_B.Win0Active & 0x2);
    GPU.GPU2D_B.Win1Active = (GPU.GPU2D_B.Win1Active & ~0x2) | (Shadow2D_B.Win1Active & 0x2);
}

void SoftRenderer::ReloadWindowCarry()
{
    Shadow2D_A.Win0Active = GPU.GPU2D_A.Win0Active & 0x2;
    Shadow2D_A.Win1Active = GPU.GPU2D_A.Win1Active & 0x2;
    Shadow2D_B.Win0Active = GPU.GPU2D_B.Win0Active & 0x2;
    Shadow2D_B.Win1Active = GPU.GPU2D_B.Win1Active & 0x2;
}

void SoftRenderer::NoteDirtyWait()
{
    if (!QueueBusy())
        return;

    Stats.DirtyWaits++;
    DirtyWaitsThisFrame++;
}

void SoftRenderer::SyncBGCaches()
{
    // Same sets, order and conditions as the 2D renderers applied in their own
    // DrawScanline. A cache is only rewritten after every queued line has been drawn,
    // because those lines must see the cache as it was when they were captured.
    if (GPU.GPU2D_A.Enabled && !GPU.GPU2D_A.ForcedBlank)
    {
        auto bgDirty = GPU.VRAMDirty_ABG.DeriveState(GPU.VRAMMap_ABG, GPU);
        auto bgExtPalDirty = GPU.VRAMDirty_ABGExtPal.DeriveState(GPU.VRAMMap_ABGExtPal, GPU);
        auto objExtPalDirty = GPU.VRAMDirty_AOBJExtPal.DeriveState(&GPU.VRAMMap_AOBJExtPal, GPU);
        if (bgDirty || bgExtPalDirty || objExtPalDirty)
        {
            NoteDirtyWait();
            Drain();
            GPU.MakeVRAMFlat_ABGCoherent(bgDirty);
            GPU.MakeVRAMFlat_ABGExtPalCoherent(bgExtPalDirty);
            GPU.MakeVRAMFlat_AOBJExtPalCoherent(objExtPalDirty);
        }
    }

    if (GPU.GPU2D_B.Enabled && !GPU.GPU2D_B.ForcedBlank)
    {
        auto bgDirty = GPU.VRAMDirty_BBG.DeriveState(GPU.VRAMMap_BBG, GPU);
        auto bgExtPalDirty = GPU.VRAMDirty_BBGExtPal.DeriveState(GPU.VRAMMap_BBGExtPal, GPU);
        auto objExtPalDirty = GPU.VRAMDirty_BOBJExtPal.DeriveState(&GPU.VRAMMap_BOBJExtPal, GPU);
        if (bgDirty || bgExtPalDirty || objExtPalDirty)
        {
            NoteDirtyWait();
            Drain();
            GPU.MakeVRAMFlat_BBGCoherent(bgDirty);
            GPU.MakeVRAMFlat_BBGExtPalCoherent(bgExtPalDirty);
            GPU.MakeVRAMFlat_BOBJExtPalCoherent(objExtPalDirty);
        }
    }
}

void SoftRenderer::SyncOBJCaches()
{
    if (GPU.GPU2D_A.Enabled)
    {
        auto objDirty = GPU.VRAMDirty_AOBJ.DeriveState(GPU.VRAMMap_AOBJ, GPU);
        if (objDirty)
        {
            NoteDirtyWait();
            Drain();
            GPU.MakeVRAMFlat_AOBJCoherent(objDirty);
        }
    }

    if (GPU.GPU2D_B.Enabled)
    {
        auto objDirty = GPU.VRAMDirty_BOBJ.DeriveState(GPU.VRAMMap_BOBJ, GPU);
        if (objDirty)
        {
            NoteDirtyWait();
            Drain();
            GPU.MakeVRAMFlat_BOBJCoherent(objDirty);
        }
    }
}

void SoftRenderer::CaptureLine(SoftLineRecord& rec, u32 line)
{
    rec.Kind = SoftLineRecord::Kind_Line;
    rec.HasScanline = true;
    rec.HasSprites = false;
    rec.ScreensEnabled = GPU.ScreensEnabled;
    rec.AbortFrame = GPU.GPU3D.AbortFrame;
    rec.CaptureEnable = GPU.CaptureEnable;

    // the position used for drawing operations is based on VCOUNT
    rec.VCount = GPU.VCount;
    rec.RenderXPos = GPU.GPU3D.RenderXPos;
    rec.MasterBrightnessA = GPU.MasterBrightnessA;
    rec.MasterBrightnessB = GPU.MasterBrightnessB;
    rec.SpriteLine = 0;

    u32 dstoffset = 256 * line;
    if (GPU.ScreenSwap)
    {
        rec.DstA = &Framebuffer[BackBuffer][0][dstoffset];
        rec.DstB = &Framebuffer[BackBuffer][1][dstoffset];
    }
    else
    {
        rec.DstA = &Framebuffer[BackBuffer][1][dstoffset];
        rec.DstB = &Framebuffer[BackBuffer][0][dstoffset];
    }

    SoftRenderer2D::CaptureState(GPU.GPU2D_A, rec.Engine[0]);
    SoftRenderer2D::CaptureState(GPU.GPU2D_B, rec.Engine[1]);
    // Backdrop writes have separate dirty bits and can include the next colour.
    if (GPU.PaletteDirty & ~0x50u)
        ++PaletteGeneration;
    GPU.PaletteDirty = 0;
    // Reuse a consumed slot's immutable bytes until guest writes change them.
    if (rec.PaletteGeneration != PaletteGeneration)
    {
        memcpy(rec.Palette, GPU.Palette, sizeof(rec.Palette));
        rec.PaletteGeneration = PaletteGeneration;
    }
    else
    {
        memcpy(rec.Palette, GPU.Palette, sizeof(u32));
        memcpy(rec.Palette + 0x400, GPU.Palette + 0x400, sizeof(u32));
    }
}

void SoftRenderer::CaptureSprites(SoftLineRecord& rec, u32 line)
{
    rec.HasSprites = true;
    rec.SpriteLine = line;
    if (GPU.OAMDirty)
    {
        ++OAMGeneration;
        GPU.OAMDirty = 0;
    }
    if (rec.OAMGeneration != OAMGeneration)
    {
        memcpy(rec.OAM, GPU.OAM, sizeof(rec.OAM));
        rec.OAMGeneration = OAMGeneration;
    }
}

void SoftRenderer::RenderRecord(SoftLineRecord& rec)
{
    SoftRenderer2D::ApplyState(rec.Engine[0], Shadow2D_A);
    SoftRenderer2D::ApplyState(rec.Engine[1], Shadow2D_B);
    Soft2D[0]->SetLineMemory(rec.Palette, rec.OAM);
    Soft2D[1]->SetLineMemory(rec.Palette, rec.OAM);

    if (rec.HasScanline)
        RenderScanline(rec);

    if (rec.HasSprites)
    {
        Soft2D[0]->DrawSprites(rec.SpriteLine);
        Soft2D[1]->DrawSprites(rec.SpriteLine);
    }
}

void SoftRenderer::DrawScanline(u32 line)
{
    // GPU::StartHBlank calls DrawSprites right after DrawScanline for lines 0..190, so a
    // record left open here means that call never came.
    if (OpenRecord)
        CommitRecord(false);

    const bool visible = GPU.VCount < 192;
    if (visible)
        SyncBGCaches();

    // Display capture and the VRAM and FIFO display modes read or write live memory at
    // this exact point of the frame, so such a line is drawn right here.
    const bool needsLiveMemory = visible &&
        (GPU.CaptureEnable || ((GPU.GPU2D_A.DispCnt >> 16) & 0x3) >= 2);

    if (!Worker || needsLiveMemory || DirtyWaitsThisFrame > MaxDirtyWaitsPerFrame)
    {
        Drain();
        CaptureLine(InlineRecord, line);
        RenderRecord(InlineRecord);
        MergeWindowCarry();
        Stats.InlineLines++;
        return;
    }

    SoftLineRecord& rec = AcquireRecord();
    CaptureLine(rec, line);
    OpenRecord = &rec;

    // Line 191 gets no DrawSprites call. It also ends the visible frame, so whatever is
    // still batched goes out now instead of waiting for the next drain.
    if (line >= 191)
        CommitRecord(true);
}

void SoftRenderer::DrawSprites(u32 line)
{
    // This may drain, which commits an open record without sprites.
    SyncOBJCaches();

    if (OpenRecord)
    {
        // Same HBlank as the open record's scanline: nothing has run in between, so its
        // captured register state is also the state these sprites are drawn with.
        CaptureSprites(*OpenRecord, line);
        CommitRecord(false);
        return;
    }

    Drain();

    InlineRecord.Kind = SoftLineRecord::Kind_Line;
    InlineRecord.HasScanline = false;
    SoftRenderer2D::CaptureState(GPU.GPU2D_A, InlineRecord.Engine[0]);
    SoftRenderer2D::CaptureState(GPU.GPU2D_B, InlineRecord.Engine[1]);
    CaptureSprites(InlineRecord, line);
    RenderRecord(InlineRecord);
    Stats.InlineSprites++;
}

void SoftRenderer::Start3DRendering()
{
    // The 3D renderer is about to overwrite the scanlines queued lines still read.
    Drain();
    Renderer::Start3DRendering();
}

void SoftRenderer::Restart3DRendering()
{
    // RestartFrame resets the scanline semaphore queued lines consume.
    Drain();
    Renderer::Restart3DRendering();
}

void SoftRenderer::SwapBuffers()
{
    Drain();
    Renderer::SwapBuffers();
    DirtyWaitsThisFrame = 0;
}

void SoftRenderer::RenderScanline(const SoftLineRecord& rec)
{
    u32* dstA = rec.DstA;
    u32* dstB = rec.DstB;

    u32 line = rec.VCount;
    if (line < 192)
    {
        // retrieve 3D output
        Output3D = Soft3D->GetLine(line, rec.RenderXPos, rec.AbortFrame);

        // draw BG/OBJ layers
        Soft2D[0]->DrawScanline(line);
        Soft2D[1]->DrawScanline(line);

        // draw the final screen output
        DrawScanlineA(line, dstA, rec.MasterBrightnessA);
        DrawScanlineB(line, dstB, rec.MasterBrightnessB);

        // perform display capture if enabled
        if (rec.CaptureEnable)
            DoCapture(line);
    }
    else
    {
        // if scanlines outside VCOUNT range 0..191 were to be visible, fill them white
        // this may happen if VCOUNT is written to during active display
        // the actual hardware behavior depends on the screen model, and suggests that
        // no video signal is output for such scanlines

        for (int i = 0; i < 256; i++)
        {
            dstA[i] = 0x3F3F3F;
            dstB[i] = 0x3F3F3F;
        }
    }

    if (rec.ScreensEnabled)
    {
        // expand the color from 6-bit to 8-bit
        ExpandColor(dstA);
        ExpandColor(dstB);
    }
    else
    {
        // if the screens are disabled: fill the framebuffer black
        for (int i = 0; i < 256; i++)
        {
            dstA[i] = 0xFF000000;
            dstB[i] = 0xFF000000;
        }
    }
}

void SoftRenderer::DrawScanlineA(u32 line, u32* dst, u16 masterBrightness)
{
    u32 dispcnt = Shadow2D_A.DispCnt;
    switch ((dispcnt >> 16) & 0x3)
    {
    case 0: // screen off
        {
            for (int i = 0; i < 256; i++)
                dst[i] = 0x3F3F3F;
        }
        return;

    case 1: // regular display
        {
            for (int i = 0; i < 256; i+=2)
                *(u64*)&dst[i] = *(u64*)&Output2D[0][i];
        }
        break;

    // The two modes below read live memory. DrawScanline never queues such a line, so
    // they only ever run on the emulation thread.
    case 2: // VRAM display
        {
            u32 vrambank = (dispcnt >> 18) & 0x3;
            if (GPU.VRAMMap_LCDC & (1<<vrambank))
            {
                u16* vram = (u16*)GPU.VRAM[vrambank];
                vram = &vram[line * 256];

                for (int i = 0; i < 256; i++)
                {
                    u16 color = vram[i];
                    u8 r = (color & 0x001F) << 1;
                    u8 g = (color & 0x03E0) >> 4;
                    u8 b = (color & 0x7C00) >> 9;

                    dst[i] = r | (g << 8) | (b << 16);
                }
            }
            else
            {
                for (int i = 0; i < 256; i++)
                    dst[i] = 0;
            }
        }
        break;

    case 3: // FIFO display
        {
            for (int i = 0; i < 256; i++)
            {
                u16 color = GPU.DispFIFOBuffer[i];
                u8 r = (color & 0x001F) << 1;
                u8 g = (color & 0x03E0) >> 4;
                u8 b = (color & 0x7C00) >> 9;

                dst[i] = r | (g << 8) | (b << 16);
            }
        }
        break;
    }

    ApplyMasterBrightness(masterBrightness, dst);
}

void SoftRenderer::DrawScanlineB(u32 line, u32* dst, u16 masterBrightness)
{
    u32 dispcnt = Shadow2D_B.DispCnt;
    switch ((dispcnt >> 16) & 0x1)
    {
    case 0: // screen off
        {
            for (int i = 0; i < 256; i++)
                dst[i] = 0xFF3F3F3F;
        }
        return;

    case 1: // regular display
        {
            for (int i = 0; i < 256; i+=2)
                *(u64*)&dst[i] = *(u64*)&Output2D[1][i];
        }
        break;
    }

    ApplyMasterBrightness(masterBrightness, dst);
}

void SoftRenderer::DoCapture(u32 line)
{
    u32 captureCnt = GPU.CaptureCnt;

    u32 width, height;
    u32 sz = (captureCnt >> 20) & 0x3;
    if (sz == 0)
    {
        width = 128;
        height = 128;
    }
    else
    {
        width = 256;
        height = 64 * sz;
    }

    if (line >= height)
        return;

    u32 dstvram = (captureCnt >> 16) & 0x3;
    if (!(GPU.VRAMMap_LCDC & (1<<dstvram)))
        return;

    u16* dst = (u16*)GPU.VRAM[dstvram];
    u32 dstaddr = (((captureCnt >> 18) & 0x3) << 14) + (line * width);
    dst += (dstaddr & 0xFFFF);

    u32* srcA;
    if (captureCnt & (1<<24))
        srcA = Output3D;
    else
        srcA = Output2D[0];

    u16* srcB = nullptr;
    if (captureCnt & (1<<25))
        srcB = GPU.DispFIFOBuffer;
    else
    {
        u32 dispcnt = GPU.GPU2D_A.DispCnt;
        u32 srcvram = (dispcnt >> 18) & 0x3;
        if (GPU.VRAMMap_LCDC & (1<<srcvram))
        {
            srcB = (u16*)GPU.VRAM[srcvram];

            u32 offset = line * 256;
            if (((dispcnt >> 16) & 0x3) != 2)
                offset += (((captureCnt >> 26) & 0x3) << 14);

            srcB += (offset & 0xFFFF);
        }
    }

    static_assert(VRAMDirtyGranularity == 512);
    GPU.VRAMDirty[dstvram][(dstaddr * 2) / VRAMDirtyGranularity] = true;

    switch ((captureCnt >> 29) & 0x3)
    {
    case 0: // source A
        {
            for (u32 i = 0; i < width; i++)
            {
                u32 val = srcA[i];

                u32 r = (val >> 1) & 0x1F;
                u32 g = (val >> 9) & 0x1F;
                u32 b = (val >> 17) & 0x1F;
                u32 a = ((val >> 24) != 0) ? 0x8000 : 0;

                dst[i] = r | (g << 5) | (b << 10) | a;
            }
        }
        break;

    case 1: // source B
        {
            if (srcB)
            {
                for (u32 i = 0; i < width; i++)
                    dst[i] = srcB[i];
            }
            else
            {
                for (u32 i = 0; i < width; i++)
                    dst[i] = 0;
            }
        }
        break;

    case 2: // sources A+B
    case 3:
        {
            u32 eva = captureCnt & 0x1F;
            u32 evb = (captureCnt >> 8) & 0x1F;

            // checkme
            if (eva > 16) eva = 16;
            if (evb > 16) evb = 16;

            if (srcB)
            {
                for (u32 i = 0; i < width; i++)
                {
                    u32 val = srcA[i];

                    u32 rA = (val >> 1) & 0x1F;
                    u32 gA = (val >> 9) & 0x1F;
                    u32 bA = (val >> 17) & 0x1F;
                    u32 aA = ((val >> 24) != 0) ? 1 : 0;

                    val = srcB[i];

                    u32 rB = val & 0x1F;
                    u32 gB = (val >> 5) & 0x1F;
                    u32 bB = (val >> 10) & 0x1F;
                    u32 aB = val >> 15;

                    u32 rD = ((rA * aA * eva) + (rB * aB * evb) + 8) >> 4;
                    u32 gD = ((gA * aA * eva) + (gB * aB * evb) + 8) >> 4;
                    u32 bD = ((bA * aA * eva) + (bB * aB * evb) + 8) >> 4;
                    u32 aD = (eva>0 ? aA : 0) | (evb>0 ? aB : 0);

                    if (rD > 0x1F) rD = 0x1F;
                    if (gD > 0x1F) gD = 0x1F;
                    if (bD > 0x1F) bD = 0x1F;

                    dst[i] = rD | (gD << 5) | (bD << 10) | (aD << 15);
                }
            }
            else
            {
                for (u32 i = 0; i < width; i++)
                {
                    u32 val = srcA[i];

                    u32 rA = (val >> 1) & 0x1F;
                    u32 gA = (val >> 9) & 0x1F;
                    u32 bA = (val >> 17) & 0x1F;
                    u32 aA = ((val >> 24) != 0) ? 1 : 0;

                    u32 rD = ((rA * aA * eva) + 8) >> 4;
                    u32 gD = ((gA * aA * eva) + 8) >> 4;
                    u32 bD = ((bA * aA * eva) + 8) >> 4;
                    u32 aD = (eva>0 ? aA : 0);

                    dst[i] = rD | (gD << 5) | (bD << 10) | (aD << 15);
                }
            }
        }
        break;
    }
}

void SoftRenderer::ApplyMasterBrightness(u16 regval, u32* dst)
{
    u16 mode = regval >> 14;
    if (mode == 1)
    {
        // up
        u32 factor = regval & 0x1F;
        if (factor > 16) factor = 16;

        for (int i = 0; i < 256; i++)
            dst[i] = ColorBrightnessUp(dst[i], factor, 0x0);
    }
    else if (mode == 2)
    {
        // down
        u32 factor = regval & 0x1F;
        if (factor > 16) factor = 16;

        for (int i = 0; i < 256; i++)
            dst[i] = ColorBrightnessDown(dst[i], factor, 0xF);
    }
}

void SoftRenderer::ExpandColor(u32* dst)
{
    // convert to 32-bit BGRA
    // note: 32-bit RGBA would be more straightforward, but
    // BGRA seems to be more compatible (Direct2D soft, cairo...)
    for (int i = 0; i < 256; i+=2)
    {
        u64 c = *(u64*)&dst[i];

        u64 r = (c << 18) & 0xFC000000FC0000;
        u64 g = (c << 2) & 0xFC000000FC00;
        u64 b = (c >> 14) & 0xFC000000FC;
        c = r | g | b;

        *(u64*)&dst[i] = c | ((c & 0x00C0C0C000C0C0C0) >> 6) | 0xFF000000FF000000;
    }
}


bool SoftRenderer::GetFramebuffers(void** top, void** bottom)
{
    int frontbuf = BackBuffer ^ 1;
    *top = Framebuffer[frontbuf][0];
    *bottom = Framebuffer[frontbuf][1];
    return true;
}

}
