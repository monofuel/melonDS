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

#ifndef GPU_SOFT_H
#define GPU_SOFT_H

#include <atomic>

#include "GPU.h"
#include "GPU2D_Soft.h"
#include "GPU3D_Soft.h"
#include "Platform.h"

namespace melonDS
{

// Everything the renderer reads to draw one HBlank's work: the scanline at VCount and the
// sprites of the following line. It is filled on the emulation thread and is immutable
// once committed, so a queued line renders exactly what an immediate render would have.
struct SoftLineRecord
{
    enum : u8
    {
        Kind_Line,
        Kind_Fence, // the worker acknowledges on Sema_Fence once everything before it is drawn
        Kind_Stop,  // the worker exits
    };

    u8 Kind;
    bool HasScanline;
    bool HasSprites;
    bool ScreensEnabled;
    bool AbortFrame;
    bool CaptureEnable;

    u16 VCount;
    u16 RenderXPos;
    u16 MasterBrightnessA;
    u16 MasterBrightnessB;
    u32 SpriteLine;

    u32* DstA;
    u32* DstB;

    Soft2DState Engine[2];

    alignas(8) u8 Palette[2*1024];
    alignas(8) u8 OAM[2*1024];
};

// Counters for tests and diagnostics. Only the emulation thread writes them.
struct SoftBatch2DStats
{
    u64 QueuedLines;   // HBlank records handed to the worker
    u64 InlineLines;   // scanlines drawn on the emulation thread
    u64 InlineSprites; // DrawSprites calls drawn on the emulation thread
    u64 Publishes;     // batches made visible to the worker
    u64 Fences;        // waits for the worker to go idle
    u64 DirtyWaits;    // fences caused by a VRAM cache update with lines still queued
    u64 FullWaits;     // fences caused by a full ring
};

class SoftRenderer : public Renderer
{
public:
    explicit SoftRenderer(melonDS::NDS& nds);
    ~SoftRenderer() override;
    bool Init() override { return true; }
    void Reset() override;
    void Stop() override;

    void PreSavestate() override;
    void PostSavestate() override;

    void SetRenderSettings(RendererSettings& settings) override;

    void DrawScanline(u32 line) override;
    void DrawSprites(u32 line) override;

    void Start3DRendering() override;
    void Restart3DRendering() override;

    void VBlank() override {};
    void VBlankEnd() override {};

    void AllocCapture(u32 bank, u32 start, u32 len) override {};
    void SyncVRAMCapture(u32 bank, u32 start, u32 len, bool complete) override {};

    bool GetFramebuffers(void** top, void** bottom) override;
    void SwapBuffers() override;

private:
    friend class SoftRenderer2D;
    friend class SoftRenderer3D;
    friend struct SoftRendererTestAccess;

    // One slot always stays free for a fence or stop record.
    static constexpr u32 RingSize = 32;
    static constexpr u32 RingMask = RingSize - 1;
    static constexpr u32 DefaultBatchSize = 16;
    static constexpr u32 MaxBatchSize = 16;

    // After this many waits forced by mid-frame VRAM cache updates, the rest of the frame
    // is drawn on the emulation thread: a wake and a wait per line costs more than it saves.
    static constexpr u32 MaxDirtyWaitsPerFrame = 4;

    u32* Framebuffer[2][2];

    u32* Output3D;
    alignas(8) u32 Output2D[2][256];

    // The 2D renderers draw from these, never from GPU.GPU2D_A/B. Each line's captured
    // state is applied to them by whichever thread draws that line.
    melonDS::GPU2D Shadow2D_A;
    melonDS::GPU2D Shadow2D_B;
    SoftRenderer2D* Soft2D[2];
    SoftRenderer3D* Soft3D;

    // Lines per wake of the worker. Zero draws every line on the emulation thread.
    u32 BatchSize;
    Platform::Thread* Worker = nullptr;

    // Posted once per publish. The worker draws everything published each time it wakes.
    Platform::Semaphore* Sema_Work;
    // Posted exactly once per fence record and waited on exactly once by its sender.
    Platform::Semaphore* Sema_Fence;

    // Emulation thread only.
    u32 QueueWrite = 0;         // records committed, published or not
    u32 QueuePublishedLocal = 0;
    SoftLineRecord* OpenRecord = nullptr; // scanline captured, waiting for its DrawSprites
    u32 DirtyWaitsThisFrame = 0;
    SoftBatch2DStats Stats {};

    alignas(64) std::atomic<u32> QueuePublished {0}; // written by the emulation thread
    alignas(64) std::atomic<u32> QueueConsumed {0};  // written by the worker

    alignas(64) SoftLineRecord InlineRecord;
    SoftLineRecord Ring[RingSize];

    void StartWorker();
    void StopWorker();
    void WorkerFunc();

    bool QueueBusy() const;
    SoftLineRecord& AcquireRecord();
    void CommitRecord(bool publish);
    void PublishRecords();
    void WaitIdle();
    void Drain();

    void MergeWindowCarry();
    void ReloadWindowCarry();

    void SyncBGCaches();
    void SyncOBJCaches();
    void NoteDirtyWait();

    void CaptureLine(SoftLineRecord& rec, u32 line);
    void CaptureSprites(SoftLineRecord& rec, u32 line);
    void RenderRecord(SoftLineRecord& rec);
    void RenderScanline(const SoftLineRecord& rec);

    void DrawScanlineA(u32 line, u32* dst, u16 masterBrightness);
    void DrawScanlineB(u32 line, u32* dst, u16 masterBrightness);

    void DoCapture(u32 line);

    void ApplyMasterBrightness(u16 regval, u32* dst);
    void ExpandColor(u32* dst);
};

}

#endif // GPU_SOFT_H
