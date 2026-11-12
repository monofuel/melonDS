// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <array>
#include <memory>
#include "Args.h"
#include "ARMJIT.h"
#include "NDS.h"

using namespace melonDS;
using namespace Arm64Gen;

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); abort(); } } while (0)

enum class ExitCase { Loop, Invalidate, WrongTag, Interrupt, ShortTarget, Outside, NegativeCycles };

static void RunCase(NDS& nds, u32 num, bool thumb, ExitCase mode, bool throughCpu)
{
    fprintf(stderr, "CASE cpu=%u thumb=%d mode=%d outer=%d\n", num, thumb, (int)mode, throughCpu);
    auto& compiler = nds.JIT.JITCompiler;
    ARM& cpu = num ? static_cast<ARM&>(nds.ARM7) : static_cast<ARM&>(nds.ARM9);
    u64& timestamp = num ? nds.ARM7Timestamp : nds.ARM9Timestamp;
    u64& target = num ? nds.ARM7Target : nds.ARM9Target;
    const u32 base = num ? 0x03800000 : 0xFFFF0000;
    const u32 address = base + (thumb ? 2 : 4);
    const u32 initialFlags = 0x1F | (thumb ? 0x20 : 0);
    const u32 changedFlags = initialFlags | 0x60000000;
    std::array<u64, 32> table;
    table.fill((u64)UINT32_MAX << 32);
    timestamp = mode == ExitCase::NegativeCycles ? 100 : 0;
    target = mode == ExitCase::NegativeCycles ? 110 : 70;
    cpu.StopExecution = 0;
    cpu.Cycles = 0;
    cpu.CPSR = initialFlags;
    cpu.R[0] = cpu.R[1] = 0;
    cpu.R[15] = address + (thumb ? 2 : 4);
    cpu.FastBlockLookup = table.data();
    cpu.FastBlockLookupStart = base;
    cpu.FastBlockLookupSize = 64;

    nds.JIT.JitEnableWrite();
    const JitBlockEntry block = (JitBlockEntry)compiler.GetRXPtr();
    compiler.LDR(INDEX_UNSIGNED, W1, RCPU, offsetof(ARM, R[0]));
    compiler.ADD(W1, W1, 1);
    compiler.STR(INDEX_UNSIGNED, W1, RCPU, offsetof(ARM, R[0]));
    compiler.LDR(INDEX_UNSIGNED, W1, RCPU, offsetof(ARM, CPSR));
    compiler.STR(INDEX_UNSIGNED, W1, RCPU, offsetof(ARM, R[1]));
    compiler.MOVI2R(RCPSR, changedFlags);
    if (mode == ExitCase::NegativeCycles)
        compiler.SUB(RCycles, RCycles, 2);
    else
        compiler.ADD(RCycles, RCycles, 7);
    if (mode == ExitCase::Invalidate || mode == ExitCase::NegativeCycles)
    {
        compiler.MOVP2R(X0, &table[(address - base) / 2]);
        compiler.MOVI2R(X1, (u64)UINT32_MAX << 32);
        compiler.STR(INDEX_UNSIGNED, X1, X0, 0);
    }
    if (mode == ExitCase::Interrupt)
    {
        compiler.MOVI2R(W1, 1);
        compiler.STRB(INDEX_UNSIGNED, W1, RCPU, offsetof(ARM, IRQ));
    }
    if (mode == ExitCase::ShortTarget)
    {
        compiler.MOVP2R(X0, &target);
        compiler.MOVI2R(X1, 5);
        compiler.STR(INDEX_UNSIGNED, X1, X0, 0);
    }
    if (mode == ExitCase::Outside)
    {
        compiler.MOVI2R(W1, base + 64 + (thumb ? 2 : 4));
        compiler.STR(INDEX_UNSIGNED, W1, RCPU, offsetof(ARM, R[15]));
    }
    compiler.QuickTailCall(X0, compiler.DispatchTail[num]);
    compiler.FlushIcache();
    const u32 tag = address | (mode == ExitCase::WrongTag ? 1 - num : num);
    table[(address - base) / 2] = ((u64)tag << 32) | compiler.SubEntryOffset(block);
    nds.JIT.JitEnableExecute();

    if (throughCpu)
    {
        if (num)
            nds.ARM7.Execute<CPUExecuteMode::JIT>();
        else
            nds.ARM9.Execute<CPUExecuteMode::JIT>();
    }
    else
        ARM_Dispatch(&cpu, block);

    CHECK(cpu.CPSR == changedFlags);
    if (mode == ExitCase::Loop)
    {
        CHECK(cpu.R[0] == 10);
        CHECK(cpu.R[1] == changedFlags);
        CHECK(timestamp == 70 && cpu.Cycles == 0);
    }
    else if (mode == ExitCase::Interrupt)
    {
        CHECK(cpu.R[0] == 1 && cpu.IRQ == 1);
        CHECK(timestamp == 0 && cpu.Cycles == 7);
    }
    else
    {
        CHECK(cpu.R[0] == 1);
        CHECK(timestamp == (mode == ExitCase::NegativeCycles ? 98 : 7));
        CHECK(cpu.Cycles == 0);
    }
    cpu.FastBlockLookup = nullptr;
    cpu.FastBlockLookupSize = 0;
}

int main()
{
    for (bool fast : {false, true})
    {
        fprintf(stderr, "CONSTRUCT fast=%d\n", fast);
        setenv("MONOMELON_JIT_DISPATCH", fast ? "1" : "0", 1);
        NDSArgs args;
        args.JIT->FastMemory = false;
        auto nds = std::make_unique<NDS>(std::move(args));
        nds->Reset();
        fprintf(stderr, "CONSTRUCTED fast=%d\n", fast);
        for (u32 num = 0; num < 2; ++num)
        {
            for (bool thumb : {false, true})
            {
                RunCase(*nds, num, thumb, ExitCase::Loop, true);
                if (!fast)
                    continue;
                for (ExitCase mode : {ExitCase::Loop, ExitCase::Invalidate, ExitCase::WrongTag,
                    ExitCase::Interrupt, ExitCase::ShortTarget, ExitCase::Outside, ExitCase::NegativeCycles})
                    RunCase(*nds, num, thumb, mode, false);
                nds->JIT.JitEnableWrite();
                nds->JIT.ResetBlockCache();
                RunCase(*nds, num, thumb, ExitCase::Loop, false);
            }
        }
    }
    puts("Dispatch tests pass for ARM9/ARM7, ARM/Thumb and both exit paths.");
}
