// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <memory>
#include <random>
#include "../../src/GPU3D.cpp"

using namespace melonDS;

template<bool attribs>
static void Check(GPU3D& gpu, const Vertex* input, int count, int start)
{
    Vertex actual[10] {}, expected[10] {};
    std::copy(input, input + 10, actual);
    std::copy(input, input + 10, expected);
    const int actualCount = ClipPolygon<attribs>(gpu, actual, count, start);
    int expectedCount = ClipAgainstPlane<2, attribs>(gpu, expected, count, start);
    expectedCount = ClipAgainstPlane<1, attribs>(gpu, expected, expectedCount, start);
    expectedCount = ClipAgainstPlane<0, attribs>(gpu, expected, expectedCount, start);
    assert(actualCount == expectedCount);
    for (int i = 0; i < actualCount; i++)
    {
        for (int j = 0; j < 4; j++) assert(actual[i].Position[j] == expected[i].Position[j]);
        for (int j = 0; j < 3; j++) assert(actual[i].Color[j] == expected[i].Color[j]);
        for (int j = 0; j < 2; j++) assert(actual[i].TexCoords[j] == expected[i].TexCoords[j]);
        assert(actual[i].Clipped == expected[i].Clipped);
    }
}

int main()
{
    auto nds = std::make_unique<NDS>();
    nds->Reset();
    auto& gpu = nds->GPU.GPU3D;
    std::mt19937 random(0xD5C11F);
    Vertex vertices[10] {};
    for (int iteration = 0; iteration < 20000; iteration++)
    {
        const int count = 3 + (iteration & 1);
        const int start = (iteration & 2) ? 2 : 0;
        for (int i = 0; i < count; i++)
        {
            auto& vertex = vertices[i];
            const s32 w = iteration % 17 == 0 ? 0 : (random() & 0x1FFFFFFF);
            vertex.Position[3] = w;
            for (int axis = 0; axis < 3; axis++)
            {
                vertex.Position[axis] = static_cast<s32>(random() % (2u * w + 1)) - w;
                if ((iteration % 5) == 0) vertex.Position[axis] = (iteration & 4) ? w : -w;
                vertex.Color[axis] = random() & 0xFFFFFF;
            }
            vertex.TexCoords[0] = static_cast<s16>(random());
            vertex.TexCoords[1] = static_cast<s16>(random());
            vertex.Clipped = (iteration & 8) != 0;
        }
        // Shared strip vertices are skipped by the original clipping loops.
        if (start == 2 && iteration % 3 == 0) vertices[0].Position[0] = 0x7FFFFFFF;
        gpu.CurPolygonAttr = (iteration & 16) ? (1 << 12) : 0;
        Check<true>(gpu, vertices, count, start);
        Check<false>(gpu, vertices, count, start);
    }
    // Exercise each real clipping plane with a convex triangle.
    for (int axis = 0; axis < 3; axis++)
    {
        for (int side : {-1, 1})
        {
            for (auto& vertex : vertices)
            {
                vertex = {};
                vertex.Position[3] = 0x1000;
            }
            vertices[0].Position[0] = -0x400;
            vertices[1].Position[0] = 0x400;
            vertices[2].Position[1] = 0x400;
            vertices[2].Position[axis] = side * 0x2000;
            gpu.CurPolygonAttr = 1 << 12;
            Check<true>(gpu, vertices, 3, 0);
        }
    }
    Check<true>(gpu, vertices, 0, 0);
    Check<true>(gpu, vertices, 0, 2);
}
