// Reference CLZ helper copyright 2015 Dolphin Emulator Project (GPLv2+).
// New tests copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <time.h>
#include "dolphin/Arm64Emitter.cpp"

using namespace Arm64Gen;

struct Encoding { unsigned N, S, R; };
using Immediates = std::map<u64, Encoding>;
static constexpr u64 Seed = 0x63AD8F12B90475E1ULL;
static constexpr u64 HashStart = 14695981039346656037ULL;
static volatile u64 BenchmarkSink;

static u64 Next(u64& state)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Original r9 helper, from Dolphin Arm64Emitter.cpp (GPLv2+), commit 09556fef.
static int ReferenceClz(u64 value, int width)
{
    int count = 0;
    u64 bit = 1ULL << (width - 1);
    while (count < width && (bit & value) == 0)
    {
        ++count;
        bit >>= 1;
    }
    return count;
}

static Immediates LegalImmediates(unsigned width)
{
    // Independently enumerate encodings by decoding each repeated, rotated bit pattern.
    Immediates values;
    for (unsigned n = 0; n < 2; ++n)
    {
        for (unsigned s = 0; s < 64; ++s)
        {
            const unsigned selector = (n << 6) | ((~s) & 63);
            unsigned length = 0;
            for (unsigned bit = 1; bit <= 6; ++bit)
                if (selector & (1u << bit)) length = bit;
            if (!length) continue;
            const unsigned size = 1u << length;
            const unsigned ones = (s & (size - 1)) + 1;
            if (size > width || ones == size) continue;
            for (unsigned r = 0; r < 64; ++r)
            {
                u64 value = 0;
                for (unsigned bit = 0; bit < width; ++bit)
                    if (((bit + r) % size) < ones) value |= 1ULL << bit;
                // The first encoding has the canonical low rotation, without ignored high bits.
                values.emplace(value, Encoding{n, s, r});
            }
        }
    }
    assert(values.size() == (width == 32 ? 1302 : 5334));
    return values;
}

static void CheckImmediate(u64 value, unsigned width, const Immediates& legal)
{
    unsigned n = 0xA5, s = 0xB6, r = 0xC7;
    const auto found = legal.find(width == 32 ? value & 0xFFFFFFFFULL : value);
    const bool accepted = IsImmLogical(value, width, &n, &s, &r);
    assert(accepted == (found != legal.end()));
    if (accepted)
    {
        assert(n == found->second.N && s == found->second.S && r == found->second.R);
    }
    else
    {
        assert(n == 0xA5 && s == 0xB6 && r == 0xC7);
    }
}

static void CheckHelpers(const Immediates& legal32, const Immediates& legal64)
{
    u64 state = Seed;
    for (int width = 1; width <= 64; ++width)
    {
        assert(CountLeadingZeros(0, width) == width);
        for (unsigned bit = 0; bit < 64; ++bit)
        {
            for (u64 value : {1ULL << bit, ~(1ULL << bit), (1ULL << bit) - 1})
                assert(CountLeadingZeros(value, width) == ReferenceClz(value, width));
        }
        for (unsigned index = 0; index < 32768; ++index)
        {
            const u64 value = Next(state);
            assert(CountLeadingZeros(value, width) == ReferenceClz(value, width));
        }
    }
    for (unsigned width : {32u, 64u})
    {
        const auto& legal = width == 32 ? legal32 : legal64;
        CheckImmediate(0, width, legal);
        CheckImmediate(~0ULL, width, legal);
        for (const auto& entry : legal)
        {
            CheckImmediate(entry.first, width, legal);
            for (unsigned bit = 0; bit < width; ++bit)
                CheckImmediate(entry.first ^ (1ULL << bit), width, legal);
        }
        for (unsigned index = 0; index < 1000000; ++index)
            CheckImmediate(Next(state), width, legal);
    }
}

static void HashByte(u64& hash, u8 byte)
{
    hash = (hash ^ byte) * 1099511628211ULL;
}

static size_t EmitCase(ARM64XEmitter& emitter, u64 value, unsigned width, unsigned operation)
{
    emitter.SetCodePtr(0);
    const auto rd = width == 64 ? X3 : W3;
    const auto rn = width == 64 ? X5 : W5;
    const auto scratch = width == 64 ? X7 : W7;
    if (width == 32) value &= 0xFFFFFFFFULL;
    switch (operation)
    {
    case 0: emitter.MOVI2R(rd, value); break;
    case 1: emitter.MOVI2R(rd, value, false); break;
    case 2: emitter.ANDI2R(rd, rn, value, scratch); break;
    case 3: emitter.ORRI2R(rd, rn, value, scratch); break;
    case 4: emitter.EORI2R(rd, rn, value, scratch); break;
    case 5: emitter.ANDSI2R(rd, rn, value, scratch); break;
    }
    return emitter.GetCodeOffset();
}

static u64 Fingerprint(unsigned width, const std::vector<u64>& corpus, FILE* bytes)
{
    std::array<u8, 128> storage;
    // A fixed synthetic RX address makes PC-relative code generation reproducible; never execute it.
    ARM64XEmitter emitter(storage.data() + 16, reinterpret_cast<u8*>(uintptr_t(0x40000000)), 0);
    u64 hash = HashStart;
    for (u64 value : corpus)
    {
        for (unsigned operation = 0; operation < 6; ++operation)
        {
            storage.fill(0xA5);
            const size_t size = EmitCase(emitter, value, width, operation);
            assert(size > 0 && size <= 32 && size % 4 == 0);
            HashByte(hash, u8(size));
            if (bytes) assert(std::fputc(int(size), bytes) != EOF);
            for (size_t index = 0; index < size; ++index)
            {
                HashByte(hash, storage[16 + index]);
                if (bytes) assert(std::fputc(storage[16 + index], bytes) != EOF);
            }
            for (size_t index = 0; index < 16; ++index) assert(storage[index] == 0xA5);
            for (size_t index = 16 + size; index < storage.size(); ++index) assert(storage[index] == 0xA5);
        }
    }
    return hash;
}

static u64 CpuTime()
{
    timespec value;
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0);
    return u64(value.tv_sec) * 1000000000 + value.tv_nsec;
}

static void Benchmark(const std::vector<u64>& corpus, unsigned iterations)
{
    std::array<u8, 64> storage;
    ARM64XEmitter emitter(storage.data(), reinterpret_cast<u8*>(uintptr_t(0x40000000)), 0);
    for (unsigned width : {32u, 64u})
    {
        for (unsigned kind = 0; kind < 3; ++kind)
        {
            u64 checksum = 0;
            const u64 before = CpuTime();
            for (unsigned index = 0; index < iterations; ++index)
            {
                const u64 value = corpus[index % corpus.size()];
                if (kind == 0)
                {
                    unsigned n = 0, s = 0, r = 0;
                    checksum += IsImmLogical(value, width, &n, &s, &r) + n + s + r;
                }
                else
                {
                    const auto size = EmitCase(emitter, value, width, kind == 1 ? 0 : 2 + index % 4);
                    u32 last;
                    memcpy(&last, storage.data() + size - 4, 4);
                    checksum += size + last;
                }
            }
            const u64 elapsed = CpuTime() - before;
            BenchmarkSink = checksum;
            printf("EMITTER_BENCH kind=%s width=%u iterations=%u cpu_ns=%llu checksum=%llu\n",
                kind == 0 ? "logical" : kind == 1 ? "movi" : "logical_emit", width, iterations,
                static_cast<unsigned long long>(elapsed), static_cast<unsigned long long>(checksum));
        }
    }
}

int main(int argc, char** argv)
{
    unsigned iterations = 0;
    FILE* bytes = nullptr;
    assert(argc == 1 || argc == 3);
    if (argc == 3 && !strcmp(argv[1], "--bytes"))
    {
        bytes = std::fopen(argv[2], "wb");
        assert(bytes);
    }
    else if (argc == 3)
    {
        assert(!strcmp(argv[1], "--bench"));
        char* end;
        errno = 0;
        const auto value = std::strtoul(argv[2], &end, 10);
        assert(!errno && *argv[2] && !*end && value > 0 && value <= 100000000);
        iterations = value;
    }
    const auto legal32 = LegalImmediates(32);
    const auto legal64 = LegalImmediates(64);
    CheckHelpers(legal32, legal64);
    std::vector<u64> benchmark;
    for (unsigned width : {32u, 64u})
    {
        const auto& legal = width == 32 ? legal32 : legal64;
        std::vector<u64> corpus {0, ~0ULL, 0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL,
            0xFFFFFFFFULL, 0x80000000ULL, 0x40000000, 0x40001000, 0x3FFFFFFC};
        for (const auto& entry : legal) corpus.push_back(entry.first);
        u64 state = Seed;
        for (unsigned index = 0; index < 65536; ++index) corpus.push_back(Next(state));
        const u64 hash = Fingerprint(width, corpus, bytes);
        // Captured independently from pristine r9; full byte streams also compare exactly.
        assert(hash == (width == 32 ? 0xF4265026BCCD6E93ULL : 0x40046444E08AFF06ULL));
        printf("EMITTER_BYTES width=%u cases=%zu hash=%016llx\n", width, corpus.size() * 6,
            static_cast<unsigned long long>(hash));
        if (width == 64) benchmark = std::move(corpus);
    }
    if (bytes) assert(std::fclose(bytes) == 0);
    if (iterations) Benchmark(benchmark, iterations);
}
