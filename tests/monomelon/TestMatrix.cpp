// Reference matrix copyright 2016-2026 melonDS team.
// New tests and NEON experiment copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
// Extracted unchanged from melonDS r9 GPU3D.cpp, apart from the function name and noinline.
#include <cstdint>
#include <cstring>
using s32 = int32_t;
using s64 = int64_t;
__attribute__((noinline)) void MatrixMult4x4Reference(s32* m, s32* s)
{
    s32 tmp[16];
    memcpy(tmp, m, 16*4);

    // m = s*m
    m[0] = ((s64)s[0]*tmp[0] + (s64)s[1]*tmp[4] + (s64)s[2]*tmp[8] + (s64)s[3]*tmp[12]) >> 12;
    m[1] = ((s64)s[0]*tmp[1] + (s64)s[1]*tmp[5] + (s64)s[2]*tmp[9] + (s64)s[3]*tmp[13]) >> 12;
    m[2] = ((s64)s[0]*tmp[2] + (s64)s[1]*tmp[6] + (s64)s[2]*tmp[10] + (s64)s[3]*tmp[14]) >> 12;
    m[3] = ((s64)s[0]*tmp[3] + (s64)s[1]*tmp[7] + (s64)s[2]*tmp[11] + (s64)s[3]*tmp[15]) >> 12;

    m[4] = ((s64)s[4]*tmp[0] + (s64)s[5]*tmp[4] + (s64)s[6]*tmp[8] + (s64)s[7]*tmp[12]) >> 12;
    m[5] = ((s64)s[4]*tmp[1] + (s64)s[5]*tmp[5] + (s64)s[6]*tmp[9] + (s64)s[7]*tmp[13]) >> 12;
    m[6] = ((s64)s[4]*tmp[2] + (s64)s[5]*tmp[6] + (s64)s[6]*tmp[10] + (s64)s[7]*tmp[14]) >> 12;
    m[7] = ((s64)s[4]*tmp[3] + (s64)s[5]*tmp[7] + (s64)s[6]*tmp[11] + (s64)s[7]*tmp[15]) >> 12;

    m[8] = ((s64)s[8]*tmp[0] + (s64)s[9]*tmp[4] + (s64)s[10]*tmp[8] + (s64)s[11]*tmp[12]) >> 12;
    m[9] = ((s64)s[8]*tmp[1] + (s64)s[9]*tmp[5] + (s64)s[10]*tmp[9] + (s64)s[11]*tmp[13]) >> 12;
    m[10] = ((s64)s[8]*tmp[2] + (s64)s[9]*tmp[6] + (s64)s[10]*tmp[10] + (s64)s[11]*tmp[14]) >> 12;
    m[11] = ((s64)s[8]*tmp[3] + (s64)s[9]*tmp[7] + (s64)s[10]*tmp[11] + (s64)s[11]*tmp[15]) >> 12;

    m[12] = ((s64)s[12]*tmp[0] + (s64)s[13]*tmp[4] + (s64)s[14]*tmp[8] + (s64)s[15]*tmp[12]) >> 12;
    m[13] = ((s64)s[12]*tmp[1] + (s64)s[13]*tmp[5] + (s64)s[14]*tmp[9] + (s64)s[15]*tmp[13]) >> 12;
    m[14] = ((s64)s[12]*tmp[2] + (s64)s[13]*tmp[6] + (s64)s[14]*tmp[10] + (s64)s[15]*tmp[14]) >> 12;
    m[15] = ((s64)s[12]*tmp[3] + (s64)s[13]*tmp[7] + (s64)s[14]*tmp[11] + (s64)s[15]*tmp[15]) >> 12;
}


// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

extern void MatrixMult4x4Reference(int32_t*, int32_t*);
namespace melonDS { void MatrixMult4x4(int32_t*, int32_t*); }
using Matrix = std::array<int32_t, 16>;
using Storage = std::array<int32_t, 80>;
static uint64_t randomState = 0x75290842daf37189ull;
static volatile uint64_t sink;

static uint32_t Next()
{
    randomState ^= randomState << 13;
    randomState ^= randomState >> 7;
    randomState ^= randomState << 17;
    return uint32_t(randomState);
}
static uint64_t Now()
{
    timespec value;
    const int status = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value);
    assert(status == 0);
    return uint64_t(value.tv_sec) * 1000000000ull + value.tv_nsec;
}
static void Oracle(int32_t* m, int32_t* s)
{
    Matrix original;
    std::memcpy(original.data(), m, 64);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            uint64_t sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += uint64_t(int64_t(s[row * 4 + k])) * uint64_t(int64_t(original[k * 4 + column]));
            const uint32_t bits = uint32_t(sum >> 12);
            std::memcpy(m + row * 4 + column, &bits, sizeof(bits));
        }
}
static void Verify()
{
    const int32_t special[] = {0, 1, -1, 4096, -4096, 8192, -8192,
        std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min()};
    for (int test = 0; test < 100000; ++test)
    {
        Storage expected, reference, candidate;
        for (size_t i = 0; i < expected.size(); ++i)
            expected[i] = test < 9 ? special[(test + i) % 9] : int32_t(Next());
        const int offset = test % 49 - 24;
        reference = expected;
        candidate = expected;
        Oracle(expected.data() + 32, expected.data() + 32 + offset);
        MatrixMult4x4Reference(reference.data() + 32, reference.data() + 32 + offset);
        melonDS::MatrixMult4x4(candidate.data() + 32, candidate.data() + 32 + offset);
        assert(reference == expected && candidate == expected);
    }
    std::puts("Verified 100000 full-storage cases, including overlap, disjoint inputs and signed wrap.");
}
static void Bench(bool neon, int repeat)
{
    constexpr int iterations = 1000000;
    std::array<Matrix, 256> matrices, coefficients;
    for (size_t item = 0; item < matrices.size(); ++item)
        for (int value = 0; value < 16; ++value)
        {
            matrices[item][value] = int32_t(Next() & 0x3fff) - 8192;
            coefficients[item][value] = int32_t(Next() & 0x3fff) - 8192;
        }
    auto run = neon ? melonDS::MatrixMult4x4 : MatrixMult4x4Reference;
    uint64_t checksum = 0;
    const uint64_t before = Now();
    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        Matrix result = matrices[iteration & 255];
        run(result.data(), coefficients[iteration & 255].data());
        checksum += uint32_t(result[iteration & 15]);
    }
    const uint64_t elapsed = Now() - before;
    sink = checksum;
    std::printf("MATRIX repeat=%d variant=%s calls=%d cpu_ns=%llu ns_per_call=%.6f checksum=%llu\n",
        repeat, neon ? "neon" : "scalar", iterations,
        (unsigned long long)elapsed, double(elapsed) / iterations, (unsigned long long)checksum);
}
int main(int argc, char**)
{
    Verify();
    if (argc <= 1) return 0;
    for (int repeat = 0; repeat < 4; ++repeat)
    {
        const uint64_t seed = randomState;
        const bool first = (repeat & 1) != 0;
        Bench(first, repeat);
        randomState = seed;
        Bench(!first, repeat);
    }
}
