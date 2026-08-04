// Pass/fail guard on the Cluster collapse compensation.
//
// clip_sweep.cpp reports how hard the soft clip is working; this file asserts
// it never works at all. Everything is measured at the firmware's block size,
// sample rate and master level, with the engine's pre-clip signal recovered by
// inverting SoftClip.
//
// Built twice by run.sh:
//   FOXTAIL_CLUSTER_NORM=1  compensation on  -> nothing may reach the knee
//   FOXTAIL_CLUSTER_NORM=0  compensation off -> the witness patches MUST clip,
//                                               so a vacuous test fails loudly
//
// The witness peaks recorded below were measured with the compensation off.

#include "../foxtail_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr float  kSampleRate = 48000.f;
constexpr size_t kBlock      = 5;
constexpr float  kMaster     = 0.7f;

// Renders far below the knee so the clip never engages and the true peak is
// visible; the engine is linear ahead of SoftClip, so scaling back is exact.
constexpr float kAtten = 1.f / 32.f;

int g_fail = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok) ++g_fail;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

struct Meas
{
    float peak; // pre-clip, at kMaster
    float rms;
};

Meas Run(foxtail::FoxTailOsc& osc, foxtail::Controls c, float seconds = 0.4f)
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
            peak = std::max(peak, std::max(std::fabs(outL[j]), std::fabs(outR[j])));
            s2 += (double)outL[j] * outL[j] + (double)outR[j] * outR[j];
        }
    }

    Meas m;
    m.peak = peak / kAtten;
    m.rms  = (float)(std::sqrt(s2 / (2.0 * measure * kBlock)) / kAtten);
    return m;
}

// All eight bands up, fully filled. cfg 0 (no spread, dark tilt) is the loudest
// patch the panel can reach: with the spread global, every partial sits at
// centre and each channel carries 0.707 of the whole bank. Per-band pan used to
// allow the whole bank in ONE channel at unity, 3 dB worse; that patch is gone.
// cfg 2 is the bright tilt, whose level compensation is what it exists to test.
constexpr int kNumCfg = 3;
void SetSliders(foxtail::Controls& c, int cfg)
{
    for (int b = 0; b < foxtail::kNumBands; ++b)
        c.bandGain[b] = 1.f;
    c.spread = cfg == 1 ? 1.f : 0.f;
    c.tilt   = cfg == 2 ? 1.f : 0.f;
}

struct Witness
{
    const char* name;
    int         cfg;
    float       A, B, pos, win, f0;
    float       rawPeak; // measured with FOXTAIL_CLUSTER_NORM=0
};

// High density, high partials-per-cluster, window over the whole bank: the
// patches that slam the clip on the uncompensated engine.
// rawPeak re-measured after the tilt change (the sawtooth slope is ~5 dB
// quieter than equal-power-per-octave, so every one of these moved down).
// A=0.88 now beats A=1.00 as the worst case; it replaces the old pos=0.2
// witness, which fell to 0.43 and had stopped proving anything.
const Witness kWitnesses[] = {
    {"spread  A=0.88 B=1.00 win=1.0 pos=0.0", 1, 0.88f, 1.00f, 0.f, 1.f, 220.f, 2.230f},
    {"centred A=0.88 B=1.00 win=1.0 pos=0.0", 0, 0.88f, 1.00f, 0.f, 1.f, 220.f, 2.212f},
    {"centred A=1.00 B=1.00 win=1.0 pos=0.0", 0, 1.00f, 1.00f, 0.f, 1.f, 220.f, 2.925f},
    {"spread  A=1.00 B=1.00 win=1.0 pos=0.0", 1, 1.00f, 1.00f, 0.f, 1.f, 220.f, 2.892f},
    {"spread  A=1.00 B=1.00 win=1.0 pos=0.0 f0=55", 1, 1.00f, 1.00f, 0.f, 1.f, 55.f, 2.510f},
    {"centred A=0.75 B=1.00 win=0.8 pos=0.0", 0, 0.75f, 1.00f, 0.f, 0.8f, 220.f, 1.629f},
    // Bright tilt. Lower than dark UNCOMPENSATED, but once the Cluster
    // compensation was re-derived per tilt the compensated worst case moved to
    // bright -- so this is not decoration, it is the loud side now.
    {"bright  A=1.00 B=1.00 win=1.0 pos=0.0 f0=880", 2, 1.00f, 1.00f, 0.f, 1.f, 880.f, 1.810f},
};

foxtail::Controls WitnessControls(const Witness& w)
{
    foxtail::Controls c;
    SetSliders(c, w.cfg);
    c.mode     = foxtail::kModeCluster;
    c.pitchHz  = w.f0;
    c.position = w.pos;
    c.window   = w.win;
    c.shapeA   = w.A;
    c.shapeB   = w.B;
    return c;
}

} // namespace

int main()
{
    static foxtail::FoxTailOsc osc;
    osc.Init(kSampleRate);

    const bool  norm = FOXTAIL_CLUSTER_NORM != 0;
    const float knee = foxtail::kClipKnee;

    std::printf("clip_guard: compensation %s, knee %.2f, master %.2f, block %zu\n\n",
                norm ? "ON" : "OFF (A/B reference)", knee, kMaster, kBlock);

    // --- 1. The witnesses -----------------------------------------------------
    // Off: every one must reach the clip, or the guard below proves nothing.
    // On: every one must stay under the knee.
    std::printf("witness patches (high density / high cluster count):\n");
    for (const Witness& w : kWitnesses)
    {
        const Meas m = Run(osc, WitnessControls(w));
        char       buf[192];
        if (norm)
            std::snprintf(buf, sizeof buf, "%-44s peak %.3f < knee (was %.2f)",
                          w.name, m.peak, w.rawPeak);
        else
            std::snprintf(buf, sizeof buf, "%-44s peak %.3f >= knee, clips as expected",
                          w.name, m.peak);
        Check(norm ? m.peak < knee : m.peak >= knee, buf);
    }

    if (!norm)
    {
        std::printf("\n%s\n", g_fail ? "FAILED" : "reference build ok");
        return g_fail ? 1 : 0;
    }

    // --- 2. Nothing anywhere may reach the knee -------------------------------
    // Grid over the four Cluster shaper controls plus the loudest slider
    // configurations, then random patches for the corners a grid misses.
    std::printf("\nexhaustive sweep:\n");
    float       worstPeak = 0.f;
    std::string worstName;
    int         patches = 0;

    const float f0s[] = {55.f, 220.f, 880.f};
    for (int mode = 0; mode < 2; ++mode)
        for (int cfg = 0; cfg < kNumCfg; ++cfg)
            for (float f0 : f0s)
                for (int ia = 0; ia <= 8; ++ia)
                    for (int ib = 0; ib <= 8; ++ib)
                        for (int ip = 0; ip <= 4; ++ip)
                            for (int iw = 0; iw <= 4; ++iw)
                            {
                                foxtail::Controls c;
                                SetSliders(c, cfg);
                                c.mode     = mode;
                                c.pitchHz  = f0;
                                c.shapeA   = (float)ia / 8.f;
                                c.shapeB   = (float)ib / 8.f;
                                c.position = (float)ip / 4.f;
                                c.window   = (float)iw / 4.f;
                                const Meas m = Run(osc, c, 0.2f);
                                ++patches;
                                if (m.peak > worstPeak)
                                {
                                    worstPeak = m.peak;
                                    char buf[160];
                                    std::snprintf(buf, sizeof buf,
                                                  "%s cfg%d f0=%g A=%.2f B=%.2f pos=%.2f win=%.2f",
                                                  mode ? "shepard" : "cluster", cfg, f0,
                                                  c.shapeA, c.shapeB, c.position, c.window);
                                    worstName = buf;
                                }
                            }

    std::mt19937                          rng(20260803);
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    for (int i = 0; i < 600; ++i)
    {
        foxtail::Controls c;
        for (int b = 0; b < foxtail::kNumBands; ++b)
        {
            c.bandGain[b] = uni(rng);
            c.bandShape[b] = uni(rng);
        }
        c.tilt     = uni(rng);
        c.bandShift = uni(rng);
        c.spread   = uni(rng);
        c.inharm   = uni(rng);
        c.mode     = uni(rng) < 0.5f ? 0 : 1;
        c.pitchHz  = 30.f * std::exp2(uni(rng) * 6.f);
        c.position = uni(rng);
        c.window   = uni(rng);
        c.shapeA   = uni(rng);
        c.shapeB   = uni(rng);
        c.parity   = uni(rng) < 0.5f ? 0.f : 1.f;
        const Meas m = Run(osc, c, 0.2f);
        ++patches;
        if (m.peak > worstPeak)
        {
            worstPeak = m.peak;
            char buf[160];
            std::snprintf(buf, sizeof buf, "rand#%d %s A=%.2f B=%.2f", i,
                          c.mode ? "shepard" : "cluster", c.shapeA, c.shapeB);
            worstName = buf;
        }
    }

    char buf[224];
    std::snprintf(buf, sizeof buf, "%d patches, worst peak %.3f (%s)", patches,
                  worstPeak, worstName.c_str());
    Check(worstPeak < knee, buf);

    // --- 2b. High density, with a window long enough to see the beat ----------
    // A collapsed cluster's members sit (1-d)*f0 apart, so at d = 0.995 and
    // f0 = 55 they beat with a ~3.6 s period. The 0.2 s sweep above measures a
    // fraction of one cycle and reports a peak that is metres below the real
    // one, which is why this corner reads clean while hardware clips.
    std::printf("\nhigh density, %.0f s window:\n", 8.0);
    float worstLong = 0.f, worstCrest = 0.f;
    std::string longName;
    {
        const float f0s2[] = {55.f, 220.f};
        const float Bs[]   = {0.90f, 0.95f, 0.99f, 1.00f};
        for (float f0 : f0s2)
            for (float B : Bs)
                for (int ia = 4; ia <= 8; ++ia)
                    for (int ip = 0; ip <= 2; ++ip)
                        for (int iw = 2; iw <= 4; ++iw)
                        {
                            foxtail::Controls c;
                            SetSliders(c, 2); // whole bank in one channel
                            c.mode     = foxtail::kModeCluster;
                            c.pitchHz  = f0;
                            c.shapeA   = (float)ia / 8.f;
                            c.shapeB   = B;
                            c.position = (float)ip / 4.f;
                            c.window   = (float)iw / 4.f;
                            const Meas m = Run(osc, c, 8.f);
                            if (m.peak > worstLong)
                            {
                                worstLong  = m.peak;
                                worstCrest = m.peak / m.rms;
                                char b2[160];
                                std::snprintf(b2, sizeof b2,
                                              "f0=%g A=%.2f B=%.2f pos=%.2f win=%.2f", f0,
                                              c.shapeA, B, c.position, c.window);
                                longName = b2;
                            }
                        }
    }
    std::snprintf(buf, sizeof buf, "worst peak %.3f, crest %.1f (%s)", worstLong, worstCrest,
                  longName.c_str());
    Check(worstLong < knee, buf);

    // --- 3. Loudness across the density knob ----------------------------------
    // The point of compensating rather than just turning Cluster down: sweeping
    // knob 5 must not change how loud the patch is.
    std::printf("\nRMS across the density knob (all-up, f0=220, full window):\n");
    float lo = 1e9f, hi = 0.f;
    for (int i = 0; i <= 8; ++i)
    {
        foxtail::Controls c;
        SetSliders(c, 0);
        c.mode    = foxtail::kModeCluster;
        c.pitchHz = 220.f;
        c.window  = 1.f;
        c.shapeA  = 1.f;
        c.shapeB  = (float)i / 8.f;
        const Meas m = Run(osc, c);
        lo = std::min(lo, m.rms);
        hi = std::max(hi, m.rms);
        std::printf("    density %.2f: %6.1f dBFS  peak %.3f\n", c.shapeB,
                    20.f * std::log10(m.rms), m.peak);
    }
    const float spreadDb = 20.f * std::log10(hi / lo);
    std::snprintf(buf, sizeof buf, "density sweep holds level within %.1f dB", spreadDb);
    Check(spreadDb < 3.f, buf);

    // --- 3b. Cluster knob at 0 must be a true bypass --------------------------
    // At shapeA = 0 every cluster is one partial and nothing shifts, so the
    // density knob must have NO effect — same peak at density 0 and 1. The
    // first compensation formula failed this (span m instead of m-1: a phantom
    // -1.6 dB cut at density 1, jittering on knob noise).
    {
        foxtail::Controls c;
        SetSliders(c, 0);
        c.mode    = foxtail::kModeCluster;
        c.pitchHz = 220.f;
        c.window  = 1.f;
        c.shapeA  = 0.f;
        c.shapeB  = 0.f;
        const Meas m0 = Run(osc, c);
        c.shapeB      = 1.f;
        const Meas m1 = Run(osc, c);
        std::snprintf(buf, sizeof buf,
                      "A=0: density knob inert (peak %.4f vs %.4f)", m0.peak, m1.peak);
        Check(std::fabs(m0.peak - m1.peak) < 0.005f, buf);
    }

    // --- 4. Density 0 must be untouched ---------------------------------------
    // Knob 5 at rest means no collapse, so the compensation must be exactly 1
    // and the plain oscillator must render as it always did.
    std::printf("\nno-collapse paths:\n");
    {
        foxtail::Controls c;
        SetSliders(c, 1);
        c.mode    = foxtail::kModeCluster;
        c.pitchHz = 220.f;
        c.window  = 1.f;
        c.shapeA  = 1.f;
        c.shapeB  = 0.f;
        const Meas m = Run(osc, c);
        // 0.2434 measured with FOXTAIL_CLUSTER_NORM=0; at density 0 the
        // compensation is boost=1 and only FastRSqrt(1) = 0.9983 remains.
        // Re-measure whenever kHeadroom, the pan layout or the tilt changes --
        // all three move it, and it scales linearly with kHeadroom.
        std::snprintf(buf, sizeof buf, "density 0 untouched (peak %.4f, raw 0.2434)", m.peak);
        Check(std::fabs(m.peak - 0.2434f * 0.9983f) < 0.005f, buf);
    }
    {
        foxtail::Controls c;
        SetSliders(c, 1);
        c.mode    = foxtail::kModeShepard;
        c.pitchHz = 220.f;
        c.window  = 1.f;
        c.shapeA  = 0.5f;
        c.shapeB  = 0.f;
        const Meas m = Run(osc, c);
        std::snprintf(buf, sizeof buf, "shepard unaffected (peak %.3f)", m.peak);
        Check(m.peak < knee, buf);
    }

    std::printf("\n%s\n", g_fail ? "FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}