// Copyright 2016-2026 melonDS team.
// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
// The reference enqueue/read bodies below preserve GPU3D.cpp at a14c1f0c.
#pragma once

#include <array>
#include <cstring>
#include <type_traits>
#include <utility>

using FifoEntry = decltype(std::declval<GPU3D&>().CmdPIPE.Read());
static_assert(sizeof(FifoEntry) == 8);
static_assert(std::is_trivially_copyable<FifoEntry>::value);

static void ReferenceFifoWrite(GPU3D& gpu, const FifoEntry& entry)
{
    if (gpu.CmdFIFO.IsEmpty() && !gpu.CmdPIPE.IsFull())
        gpu.CmdPIPE.Write(entry);
    else
    {
        if (gpu.CmdFIFO.IsFull())
        {
            gpu.CmdStallQueue.Write(entry);
            gpu.NDS.GXFIFOStall();
            return;
        }
        gpu.CmdFIFO.Write(entry);
    }
    gpu.GXStat |= (1<<27);
    if (entry.Command == 0x11 || entry.Command == 0x12)
    {
        gpu.GXStat |= (1<<14);
        gpu.NumPushPopCommands++;
    }
    else if (entry.Command == 0x70 || entry.Command == 0x71 || entry.Command == 0x72)
    {
        gpu.GXStat |= (1<<0);
        gpu.NumTestCommands++;
    }
}

static FifoEntry ReferenceFifoRead(GPU3D& gpu)
{
    FifoEntry ret = gpu.CmdPIPE.Read();
    if (gpu.CmdPIPE.Level() <= 2)
    {
        if (!gpu.CmdFIFO.IsEmpty()) gpu.CmdPIPE.Write(gpu.CmdFIFO.Read());
        if (!gpu.CmdFIFO.IsEmpty()) gpu.CmdPIPE.Write(gpu.CmdFIFO.Read());
        if (!gpu.CmdStallQueue.IsEmpty())
        {
            while (!gpu.CmdStallQueue.IsEmpty())
            {
                if (gpu.CmdFIFO.IsFull()) break;
                FifoEntry entry = gpu.CmdStallQueue.Read();
                ReferenceFifoWrite(gpu, entry);
            }
            if (gpu.CmdStallQueue.IsEmpty()) gpu.NDS.GXFIFOUnstall();
        }
        gpu.CheckFIFODMA();
        gpu.CheckFIFOIRQ();
    }
    return ret;
}

class FifoValueNDS : public NDS
{
public:
    bool Observe = false;
    std::vector<std::array<u32, 4>> DmaChecks;
    void CheckDMAs(u32 cpu, u32 mode) override
    {
        if (Observe)
        {
            assert(cpu == 0 && mode == 7);
            auto& gpu = GPU.GPU3D;
            DmaChecks.push_back({gpu.CmdFIFO.Level(), gpu.CmdPIPE.Level(), gpu.CmdStallQueue.Level(), IF[0]});
        }
        NDS::CheckDMAs(cpu, mode);
    }
};

static FifoEntry InitializedEntry(u32 index)
{
    // Seed all eight bytes so stall-drain comparisons include preserved spare bytes.
    FifoEntry entry;
    const u64 pattern = 0xA5C37E0000000000ull | (static_cast<u64>(index) << 40);
    std::memcpy(&entry, &pattern, sizeof(entry));
    entry.Command = 0x20;
    entry.Param = index * 0x125u;
    return entry;
}

static void PrimeFifos(GPU3D& gpu, u32 pipe, u32 fifo, u32 stall = 0)
{
    gpu.CmdPIPE.Clear(); gpu.CmdFIFO.Clear(); gpu.CmdStallQueue.Clear();
    gpu.GXStat = (pipe || fifo || stall) ? (1u << 27) : 0;
    gpu.NumPushPopCommands = gpu.NumTestCommands = 0;
    gpu.NumCommands = gpu.CurCommand = gpu.ParamCount = gpu.TotalParams = 0;
    gpu.NDS.CPUStop &= ~CPUStop_GXStall;
    for (u32 i = 0; i < pipe; ++i) gpu.CmdPIPE.Write(InitializedEntry(i));
    for (u32 i = 0; i < fifo; ++i) gpu.CmdFIFO.Write(InitializedEntry(i + 4));
    for (u32 i = 0; i < stall; ++i) gpu.CmdStallQueue.Write(InitializedEntry(i + 260));
    if (stall) gpu.NDS.GXFIFOStall();
}

template<typename Queue>
static void CompareQueue(const Queue& actual, const Queue& reference, bool raw)
{
    assert(actual.Level() == reference.Level());
    for (u32 i = 0; i < actual.Level(); ++i)
    {
        const auto a = actual.Peek(i), b = reference.Peek(i);
        assert(a.Command == b.Command && a.Param == b.Param);
        if (raw) assert(std::memcmp(&a, &b, sizeof(a)) == 0);
    }
}

static void CompareFifos(FifoValueNDS& actual, FifoValueNDS& reference, bool raw)
{
    auto& a = actual.GPU.GPU3D;
    auto& b = reference.GPU.GPU3D;
    CompareQueue(a.CmdPIPE, b.CmdPIPE, raw);
    CompareQueue(a.CmdFIFO, b.CmdFIFO, raw);
    CompareQueue(a.CmdStallQueue, b.CmdStallQueue, raw);
    assert(a.GXStat == b.GXStat);
    assert(a.NumPushPopCommands == b.NumPushPopCommands && a.NumTestCommands == b.NumTestCommands);
    assert(actual.IF[0] == reference.IF[0]);
    assert(actual.CPUStop == reference.CPUStop && actual.ARM9Target == reference.ARM9Target);
    assert(actual.DmaChecks == reference.DmaChecks);
}

static void CheckFifoValue()
{
    auto actual = std::make_unique<FifoValueNDS>();
    auto reference = std::make_unique<FifoValueNDS>();
    actual->Reset(); reference->Reset();
    auto& a = actual->GPU.GPU3D;
    auto& b = reference->GPU.GPU3D;
    a.SetEnabled(true, true); b.SetEnabled(true, true);
    auto initial = State(a);
    Savestate load(initial.data(), initial.size(), false);
    b.DoSavestate(&load);
    assert(!load.Error);

    for (u32 pipe : {0u, 2u, 4u})
    for (u32 fifo : {0u, 1u, 127u, 128u, 255u, 256u})
    for (u8 command : {0x11, 0x12, 0x20, 0x70, 0x71, 0x72})
    {
        PrimeFifos(a, pipe, fifo); PrimeFifos(b, pipe, fifo);
        FifoEntry entry = InitializedEntry(0x42);
        entry.Command = command;
        a.Write32(0x04000400 + (command << 2), entry.Param);
        ReferenceFifoWrite(b, entry);
        // Newly decoded entries historically leave spare bytes unspecified.
        CompareFifos(*actual, *reference, false);
    }
    for (u32 pipe : {0u, 2u, 4u})
    for (u32 fifo : {0u, 127u, 128u, 255u, 256u})
    {
        PrimeFifos(a, pipe, fifo); PrimeFifos(b, pipe, fifo);
        a.Write32(0x0400043C, 0x20202020);
        for (u32 index = 0; index < 4; ++index)
        {
            const auto entry = InitializedEntry(index + 50);
            a.Write32(0x04000400 + index * 4, entry.Param);
            ReferenceFifoWrite(b, entry);
            CompareFifos(*actual, *reference, false);
        }
        assert(a.NumCommands == 0);
    }

    // Drain full queues through real ExecuteCommand, comparing the original read/write oracle.
    // Each seeded entry is COLOR, which consumes one cycle with idle geometry pipelines.
    // Synchronize unused slots before the fully initialized bytewise history comparison.
    auto beforeDrain = State(a);
    Savestate sync(beforeDrain.data(), beforeDrain.size(), false);
    b.DoSavestate(&sync);
    assert(!sync.Error);
    PrimeFifos(a, 4, 256, 3); PrimeFifos(b, 4, 256, 3);
    a.GXStat |= 1u << 30; b.GXStat |= 1u << 30;
    actual->ClearIRQ(0, IRQ_GXFIFO); reference->ClearIRQ(0, IRQ_GXFIFO);
    actual->Observe = reference->Observe = true;
    unsigned reads = 0;
    while (!a.CmdPIPE.IsEmpty())
    {
        a.CycleCount = b.CycleCount = 0;
        a.Timestamp = actual->ARM9Timestamp >> actual->ARM9ClockShift;
        b.Timestamp = reference->ARM9Timestamp >> reference->ARM9ClockShift;
        const auto entry = ReferenceFifoRead(b);
        b.VertexColor[0] = entry.Param & 31;
        b.VertexColor[1] = (entry.Param >> 5) & 31;
        b.VertexColor[2] = (entry.Param >> 10) & 31;
        b.CycleCount = 1;
        a.Run();
        assert(a.CycleCount == 1);
        CompareFifos(*actual, *reference, true);
        assert(State(a) == State(b));
        ++reads;
    }
    assert(reads == 263);
    assert(a.CmdFIFO.IsEmpty() && a.CmdStallQueue.IsEmpty());
    assert(!(actual->CPUStop & CPUStop_GXStall));
    assert(!actual->DmaChecks.empty());
    assert((actual->DmaChecks.front()[3] & (1u << IRQ_GXFIFO)) == 0);
    for (const auto& check : actual->DmaChecks) assert(check[0] < 128);
    assert(actual->IF[0] & (1u << IRQ_GXFIFO));

    // Complete bytewise queue/state history must survive save/load after the drain.
    auto saved = State(a);
    Savestate restored(saved.data(), saved.size(), false);
    b.DoSavestate(&restored);
    assert(!restored.Error && saved == State(b));
}
