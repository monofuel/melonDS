// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <algorithm>
#include <memory>
#include <vector>
#include "NDS.h"

using namespace melonDS;

class SchedulerTest : public NDS
{
public:
    using NDS::NextTarget;
    using NDS::RunSystem;
    using NDS::RunSystemSleep;

    std::vector<u32> Fired;
    u32 Action = 0;

    void Clear()
    {
        for (u32 id = 0; id < Event_MAX; ++id)
        {
            CancelEvent(id);
            RegisterEventFuncs(id, this, {Callback});
            SchedList[id].Timestamp = 0;
        }
        SysTimestamp = ARM9Timestamp = ARM7Timestamp = 0;
        CurCPU = 0;
        Fired.clear();
        Action = 0;
    }

    void CheckTarget()
    {
        u64 expected = UINT64_MAX;
        for (u32 id = 0; id < Event_MAX; ++id)
            if (SchedListMask & (1u << id))
                expected = std::min(expected, SchedList[id].Timestamp);
        const u64 maximum = SysTimestamp + 64;
        if (expected >= maximum + 8)
            expected = maximum;
        assert(NextTarget() == expected);
        assert(NextTarget() == expected);
    }

    static void Callback(void* data, u32 param)
    {
        auto& nds = *static_cast<SchedulerTest*>(data);
        nds.Fired.push_back(param);
        if (nds.Action == 5)
            nds.CheckTarget();
        if (param != 1)
            return;
        if (nds.Action == 1)
            nds.CancelEvent(Event_Sqrt);
        else if (nds.Action == 2)
        {
            nds.CancelEvent(Event_Sqrt);
            nds.ScheduleEvent(Event_Sqrt, false, 100, 0, 3);
        }
        else if (nds.Action == 3)
            nds.ScheduleEvent(Event_LCD, false, 0, 0, 4);
        else if (nds.Action == 4)
            nds.ScheduleEvent(Event_Div, true, 7, 0, 1);
    }
};

int main()
{
    auto nds = std::make_unique<SchedulerTest>();
    nds->Reset();
    nds->Clear();
    nds->CheckTarget();
    nds->ScheduleEvent(Event_Div, false, 10, 0, 1);
    nds->CheckTarget();
    nds->ScheduleEvent(Event_Sqrt, false, 5, 0, 2);
    nds->CheckTarget();
    nds->CancelEvent(Event_Sqrt);
    nds->CheckTarget();
    nds->RunSystem(9);
    assert(nds->Fired.empty());
    nds->RunSystem(10);
    assert(nds->Fired == std::vector<u32>({1}));
    nds->CheckTarget();

    // Cancellation must preserve the original traversal's captured mask.
    for (u32 action = 1; action <= 5; ++action)
    {
        nds->Clear();
        nds->Action = action;
        nds->ScheduleEvent(Event_Div, false, 10, 0, 1);
        if (action <= 2 || action == 5)
            nds->ScheduleEvent(Event_Sqrt, false, 10, 0, 2);
        nds->CheckTarget();
        nds->RunSystem(10);
        assert(nds->Fired == (action == 1 || action == 5 ? std::vector<u32>({1, 2}) : std::vector<u32>({1})));
        nds->CheckTarget();
        if (action == 3)
        {
            nds->RunSystem(10);
            assert(nds->Fired == std::vector<u32>({1, 4}));
        }
        if (action == 4)
        {
            nds->RunSystem(17);
            assert(nds->Fired == std::vector<u32>({1, 1}));
            nds->CheckTarget();
        }
    }

    // Sleep runs RTC only, while overdue ordinary events move with the clock.
    nds->Clear();
    nds->ScheduleEvent(Event_SPU, false, 5, 0, 5);
    nds->Action = 5;
    nds->ScheduleEvent(Event_RTC, false, 5, 0, 6);
    nds->ScheduleEvent(Event_Div, false, 5, 0, 7);
    nds->CheckTarget();
    nds->RunSystemSleep(20);
    assert(nds->Fired == std::vector<u32>({6}));
    assert(nds->SchedList[Event_SPU].Timestamp == 25);
    assert(nds->SchedList[Event_Div].Timestamp == 25);
    nds->CheckTarget();
    nds->RunSystem(25);
    assert(nds->Fired == std::vector<u32>({6, 5, 7}));

    // Loading must replace a previously cached earlier minimum.
    nds->Clear();
    nds->ScheduleEvent(Event_Div, false, 30, 0, 8);
    Savestate saved;
    assert(nds->DoSavestate(&saved) && !saved.Error);
    nds->CancelEvent(Event_Div);
    nds->ScheduleEvent(Event_Div, false, 2, 0, 9);
    nds->CheckTarget();
    Savestate loaded(saved.Buffer(), saved.Length(), false);
    assert(nds->DoSavestate(&loaded) && !loaded.Error);
    nds->CheckTarget();
    nds->RunSystem(30);
    assert(nds->Fired == std::vector<u32>({8}));
    nds->Reset();
    nds->CheckTarget();

    // Exercise changing minima over many schedule/cancel/fire transitions.
    nds->Clear();
    u32 random = 0x21736A4B;
    for (u32 iteration = 0; iteration < 20000; ++iteration)
    {
        random = random * 1664525 + 1013904223;
        const u32 id = (random >> 16) % Event_MAX;
        if (random & 1)
        {
            nds->CancelEvent(id);
            nds->ScheduleEvent(id, false, (random >> 8) % 128, 0, 0);
        }
        else
            nds->CancelEvent(id);
        nds->CheckTarget();
        nds->RunSystem(nds->NextTarget());
        nds->CheckTarget();
    }
}
