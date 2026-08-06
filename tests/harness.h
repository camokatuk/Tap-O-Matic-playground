// Shared rendering for the engine tests: one patch in, peak and RMS out.
//
// Everything is measured at the firmware's block size, sample rate and master
// level. Run() re-Inits the oscillator per patch, so a result never depends on
// what ran before it and sharding across cores cannot change an answer.
#pragma once

#include "../foxtail_dsp.h"

#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fxtest {

constexpr float  kSampleRate = 48000.f;
constexpr size_t kBlock      = 5;
constexpr float  kMaster     = 0.7f;

// Renders far below the knee so the clip never engages and the true peak is
// visible; the engine is linear ahead of SoftClip, so scaling back is exact.
constexpr float kAtten = 1.f / 32.f;

struct Meas
{
    float peak; // pre-clip, at kMaster
    float rms;
};

inline Meas Run(foxtail::FoxTailOsc& osc, foxtail::Controls c, float seconds = 0.4f)
{
    osc.Init(kSampleRate); // fixed phase seed: results are reproducible
    c.master = kMaster * kAtten;

    float        outL[kBlock], outR[kBlock];
    const size_t settle  = (size_t)(0.1f * kSampleRate / kBlock);
    const size_t measure = (size_t)(seconds * kSampleRate / kBlock);

    for (size_t i = 0; i < settle; ++i) osc.Process(c, outL, outR, kBlock);

    float  peak = 0.f;
    double s2   = 0;
    for (size_t i = 0; i < measure; ++i)
    {
        osc.Process(c, outL, outR, kBlock);
        for (size_t j = 0; j < kBlock; ++j)
        {
            const float l = std::fabs(outL[j]), r = std::fabs(outR[j]);
            peak = peak > l ? peak : l;
            peak = peak > r ? peak : r;
            s2 += (double)outL[j] * outL[j] + (double)outR[j] * outR[j];
        }
    }

    Meas m;
    m.peak = peak / kAtten;
    m.rms  = (float)(std::sqrt(s2 / (2.0 * measure * kBlock)) / kAtten);
    return m;
}

struct Patch
{
    foxtail::Controls c;
    std::string       name;
    float             seconds = 0.4f;
};

inline void RenderRange(const std::vector<Patch>& patches, size_t lo, size_t hi,
                        Meas* out)
{
    // Heap, not stack: FoxTailOsc is 68 KB, past a default thread stack.
    std::unique_ptr<foxtail::FoxTailOsc> osc(new foxtail::FoxTailOsc);
    for (size_t i = lo; i < hi; ++i) out[i] = Run(*osc, patches[i].c, patches[i].seconds);
}

// Renders every patch across the available cores, in patch order.
inline std::vector<Meas> RenderAll(const std::vector<Patch>& patches)
{
    std::vector<Meas> out(patches.size());
    if (patches.empty()) return out;

    size_t n = std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    if (n > patches.size()) n = patches.size();

    std::vector<std::thread> threads;
    threads.reserve(n);
    for (size_t t = 0; t < n; ++t)
        threads.emplace_back(RenderRange, std::cref(patches), patches.size() * t / n,
                             patches.size() * (t + 1) / n, out.data());
    for (std::thread& t : threads) t.join();
    return out;
}

// dBFS against the 1.0 full-scale point the clip's hard cap sits above.
inline float Db(float x) { return 20.f * std::log10(x > 1e-9f ? x : 1e-9f); }

} // namespace fxtest
