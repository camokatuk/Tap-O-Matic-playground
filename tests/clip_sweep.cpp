// Clip-engagement sweep for the Fox Tail engine.
//
// Renders a grid of panel settings plus random patches through FoxTailOsc at
// the firmware block size and sample rate, then measures how hard the soft
// clip is working. The pre-clip signal is recovered exactly by inverting the
// cubic (monotonic below its 1.5 cap), so the metrics are:
//
//   peak    pre-clip peak over the measured span
//   l1      analytic worst-case peak (sum of |amp|, all partials aligned)
//   resid   RMS of (pre - clipped) relative to output RMS, in dB
//
// An oscillator should render with resid at -inf for everything except
// deliberate extremes, which is exactly what this file exists to check. It is
// also the measurement base for any future Cluster gain-reduction curve
// (gain as a function of the shaper knobs — see todo.md).

#include "../foxtail_dsp.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include <algorithm>

namespace {

constexpr float  kSampleRate = 48000.f;
constexpr size_t kBlock      = 5;      // firmware block size — do not change
constexpr float  kMaster     = 0.7f;   // firmware's fixed master level
constexpr float  kSettleSec  = 0.15f;
constexpr float  kMeasureSec = 0.5f;

// Exact inverse of the engine's SoftClip: identity below the knee, quadratic
// solve inside it, and the hard cap reported as a lower bound on the true peak.
float InvertSoftClip(float y)
{
    const float t = foxtail::kClipKnee;
    const float s = y < 0.f ? -1.f : 1.f;
    y             = std::fabs(y);
    if (y <= t) return s * y;
    if (y >= 1.f) return s * (2.f - t); // hard-capped
    // y = x - u^2/(4(1-t)), u = x - t  ->  a*u^2 - u + (y - t) = 0
    const float a = 0.25f / (1.f - t);
    const float u = (1.f - std::sqrt(1.f - 4.f * a * (y - t))) / (2.f * a);
    return s * (t + u);
}

struct Patch
{
    foxtail::Controls c;
    std::string       name;
};

struct Result
{
    std::string name;
    float peak    = 0.f; // pre-clip
    float l1      = 0.f;
    float rmsDb   = -999.f;
    float residDb = -999.f;
    bool  hardCap = false;
};

Result RunPatch(foxtail::FoxTailOsc& osc, const Patch& p)
{
    float outL[kBlock], outR[kBlock];

    const size_t settleBlocks  = (size_t)(kSettleSec * kSampleRate / kBlock);
    const size_t measureBlocks = (size_t)(kMeasureSec * kSampleRate / kBlock);

    for (size_t i = 0; i < settleBlocks; ++i)
        osc.Process(p.c, outL, outR, kBlock);

    double sumOut2 = 0, sumResid2 = 0;
    float  peak = 0.f;
    bool   hardCap = false;

    for (size_t i = 0; i < measureBlocks; ++i)
    {
        osc.Process(p.c, outL, outR, kBlock);
        for (size_t j = 0; j < kBlock; ++j)
        {
            for (const float y : {outL[j], outR[j]})
            {
                const float x = InvertSoftClip(y);
                if (std::fabs(y) >= 1.f) hardCap = true;
                peak = std::max(peak, std::fabs(x));
                sumOut2   += (double)y * y;
                sumResid2 += (double)(x - y) * (x - y);
            }
        }
    }

    // Analytic worst case: every partial aligned. Master is a settled
    // multiplier on the amp arrays.
    float l1L = 0.f, l1R = 0.f;
    for (int k = 0; k < foxtail::kNumPartials; ++k)
    {
        l1L += std::fabs(osc.PartialAmpL(k));
        l1R += std::fabs(osc.PartialAmpR(k));
    }

    Result r;
    r.name    = p.name;
    r.peak    = peak;
    r.l1      = std::max(l1L, l1R) * kMaster;
    r.hardCap = hardCap;
    r.rmsDb   = sumOut2 > 0 ? 10.f * (float)std::log10(sumOut2 / (2.0 * measureBlocks * kBlock)) : -999.f;
    r.residDb = sumResid2 > 0 && sumOut2 > 0 ? 10.f * (float)std::log10(sumResid2 / sumOut2) : -999.f;
    return r;
}

void SetSliders(foxtail::Controls& c, int cfg)
{
    for (int b = 0; b < foxtail::kNumBands; ++b)
    {
        c.bandGain[b] = 0.f;
        c.bandPan[b]  = 0.5f;
    }
    switch (cfg)
    {
        case 0: // all up, centred
            for (int b = 0; b < foxtail::kNumBands; ++b) c.bandGain[b] = 1.f;
            break;
        case 1: // all up, hard-panned alternating — the loudest normal patch
            for (int b = 0; b < foxtail::kNumBands; ++b)
            {
                c.bandGain[b] = 1.f;
                c.bandPan[b]  = (b & 1) ? 1.f : 0.f;
            }
            break;
        case 2: c.bandGain[0] = 1.f; break;                    // fundamental band only
        case 3: c.bandGain[foxtail::kNumBands - 1] = 1.f; break; // top band only
        case 4: // descending
            for (int b = 0; b < foxtail::kNumBands; ++b)
                c.bandGain[b] = 1.f - (float)b / foxtail::kNumBands;
            break;
    }
}

const char* SliderName(int cfg)
{
    static const char* n[] = {"all-up", "hardpan", "band1", "band8", "descend"};
    return n[cfg];
}

} // namespace

int main()
{
    static foxtail::FoxTailOsc osc; // static: 64 KB sine table
    osc.Init(kSampleRate);

    std::vector<Patch> patches;

    const float shapeAs[]   = {0.f, 0.33f, 0.66f, 1.f};
    const float shapeBs[]   = {0.f, 0.5f, 0.9f, 1.f};
    const float positions[] = {0.f, 0.4f};
    const float windows[]   = {0.f, 0.5f, 1.f};
    const float f0s[]       = {55.f, 220.f, 880.f};

    for (int mode = 0; mode < 2; ++mode)
        for (int cfg = 0; cfg < 5; ++cfg)
            for (float f0 : f0s)
                for (float pos : positions)
                    for (float win : windows)
                        for (float sA : shapeAs)
                            for (float sB : shapeBs)
                            {
                                Patch p;
                                SetSliders(p.c, cfg);
                                p.c.mode     = mode;
                                p.c.pitchHz  = f0;
                                p.c.position = pos;
                                p.c.window   = win;
                                p.c.shapeA   = sA;
                                p.c.shapeB   = sB;
                                p.c.master   = kMaster;
                                char buf[128];
                                std::snprintf(buf, sizeof buf,
                                              "%s %s f0=%g pos=%.1f win=%.1f A=%.2f B=%.2f",
                                              mode ? "shepard" : "cluster",
                                              SliderName(cfg), f0, pos, win, sA, sB);
                                p.name = buf;
                                patches.push_back(p);
                            }

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    for (int i = 0; i < 1500; ++i)
    {
        Patch p;
        for (int b = 0; b < foxtail::kNumBands; ++b)
        {
            p.c.bandGain[b] = uni(rng);
            p.c.bandPan[b]  = uni(rng);
        }
        p.c.inharm   = uni(rng);
        p.c.mode     = uni(rng) < 0.5f ? 0 : 1;
        p.c.pitchHz  = 30.f * std::exp2(uni(rng) * 6.f); // 30 Hz .. ~2 kHz
        p.c.position = uni(rng);
        p.c.window   = uni(rng);
        p.c.shapeA   = uni(rng);
        p.c.shapeB   = uni(rng);
        p.c.parity   = uni(rng) < 0.5f ? 0.f : 1.f;
        p.c.master   = kMaster;
        char buf[128];
        std::snprintf(buf, sizeof buf, "rand#%d %s A=%.2f B=%.2f f0=%.0f",
                      i, p.c.mode ? "shepard" : "cluster", p.c.shapeA, p.c.shapeB,
                      p.c.pitchHz);
        p.name = buf;
        patches.push_back(p);
    }

    std::vector<Result> results;
    results.reserve(patches.size());
    for (const Patch& p : patches)
        results.push_back(RunPatch(osc, p));

    // Severity buckets on the rendered residual.
    int clean = 0, faint = 0, audible = 0, gross = 0;
    for (const Result& r : results)
    {
        if (r.residDb <= -90.f) ++clean;
        else if (r.residDb <= -60.f) ++faint;
        else if (r.residDb <= -40.f) ++audible;
        else ++gross;
    }

    std::printf("%zu patches, %.1fs measured each, block %zu, master %.2f\n\n",
                results.size(), kMeasureSec, kBlock, kMaster);
    std::printf("clip residual (rendered)\n");
    std::printf("  clean   (<= -90 dB)        %6d\n", clean);
    std::printf("  faint   (-90..-60 dB)      %6d\n", faint);
    std::printf("  audible (-60..-40 dB)      %6d\n", audible);
    std::printf("  gross   (>  -40 dB)        %6d\n\n", gross);

    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b) { return a.residDb > b.residDb; });

    std::printf("worst 15 by rendered residual:\n");
    std::printf("%-55s %5s %5s %7s %7s\n", "patch", "peak", "l1", "rms dB", "resid");
    for (size_t i = 0; i < 15 && i < results.size(); ++i)
    {
        const Result& r = results[i];
        std::printf("%-55s %5.2f %5.2f %7.1f %7.1f%s\n",
                    r.name.c_str(), r.peak, r.l1, r.rmsDb, r.residDb,
                    r.hardCap ? " HARDCAP" : "");
    }

    // Loudness across the density travel: the raw material for a knob-based
    // Cluster gain curve.
    std::printf("\nlevel check (all-up centred, f0=220, full window, pos=0):\n");
    for (float d : {0.f, 0.5f, 0.9f, 1.f})
    {
        Patch p;
        SetSliders(p.c, 0);
        p.c.mode = foxtail::kModeCluster;
        p.c.pitchHz = 220.f;
        p.c.window = 1.f;
        p.c.shapeA = 1.f;
        p.c.shapeB = d;
        p.c.master = kMaster;
        p.name = "level";
        Result r = RunPatch(osc, p);
        std::printf("  density %.2f: rms %6.1f dBFS  peak %.2f  resid %6.1f dB\n",
                    d, r.rmsDb, r.peak, r.residDb);
    }
    return 0;
}