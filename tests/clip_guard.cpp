// Pass/fail guard on the Cluster collapse compensation.
//
// Asserts the soft clip never works at all. Rendering and measurement come from
// harness.h, at the firmware's block size, sample rate and master level.
//
// Built twice by run.sh:
//   FOXTAIL_CLUSTER_NORM=1  compensation on  -> nothing may reach the knee
//   FOXTAIL_CLUSTER_NORM=0  compensation off -> the witness patches MUST clip,
//                                               so a vacuous test fails loudly
//
// The witness peaks recorded below were measured with the compensation off.

#include "harness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using fxtest::kBlock;
using fxtest::kMaster;
using fxtest::kSampleRate;
using fxtest::Meas;
using fxtest::Patch;
using fxtest::Run;

int g_fail = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok) ++g_fail;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

// The sweeps report a single worst patch; harness.h renders them all.
struct Worst
{
    float  peak = 0.f;
    float  rms  = 0.f;
    size_t idx  = (size_t)-1;
};

Worst RenderWorst(const std::vector<Patch>& patches)
{
    const std::vector<Meas> m = fxtest::RenderAll(patches);
    Worst best;
    for (size_t i = 0; i < m.size(); ++i)
        if (m[i].peak > best.peak) // ties keep the lowest index, so the reported
        {                          // patch does not depend on the shard count
            best.peak = m[i].peak;
            best.rms  = m[i].rms;
            best.idx  = i;
        }
    return best;
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
// A=0.95 now beats A=1.00 as the worst case; it replaces the old pos=0.2
// witness, which fell to 0.43 and had stopped proving anything.
// A is in KNOB travel, and knob 4 became centre-neutral: 0.5 is m=1, and the
// old A=0.88/0.75 spectra now sit at 0.95/0.89. Values below 0.5 are the same
// cluster sizes collapsing onto the cluster's top instead, which lands the pile
// where both tilts are quieter — the grid sweep covers that side, but it does
// not clip, so it earns no witness.
const Witness kWitnesses[] = {
    {"spread  A=0.95 B=1.00 win=1.0 pos=0.0", 1, 0.95f, 1.00f, 0.f, 1.f, 220.f, 2.230f},
    {"centred A=0.95 B=1.00 win=1.0 pos=0.0", 0, 0.95f, 1.00f, 0.f, 1.f, 220.f, 2.212f},
    {"centred A=1.00 B=1.00 win=1.0 pos=0.0", 0, 1.00f, 1.00f, 0.f, 1.f, 220.f, 2.925f},
    {"spread  A=1.00 B=1.00 win=1.0 pos=0.0", 1, 1.00f, 1.00f, 0.f, 1.f, 220.f, 2.892f},
    {"spread  A=1.00 B=1.00 win=1.0 pos=0.0 f0=55", 1, 1.00f, 1.00f, 0.f, 1.f, 55.f, 2.510f},
    {"centred A=0.89 B=1.00 win=0.8 pos=0.0", 0, 0.89f, 1.00f, 0.f, 0.8f, 220.f, 1.629f},
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
    std::vector<Patch> sweep;

    const float f0s[] = {55.f, 220.f, 880.f};
    for (int mode = 0; mode < 2; ++mode)
        for (int cfg = 0; cfg < kNumCfg; ++cfg)
            for (float f0 : f0s)
                for (int ia = 0; ia <= 8; ++ia)
                    for (int ib = 0; ib <= 8; ++ib)
                    {
                        // Shepard reads shapeB as a plain attenuation of the
                        // windowed partials (winGain = 1 - shapeB), so only the
                        // ends of its travel can be the loud case.
                        if (mode == foxtail::kModeShepard && ib != 0 && ib != 8)
                            continue;
                        for (int ip = 0; ip <= 4; ++ip)
                            for (int iw = 0; iw <= 4; ++iw)
                            {
                                Patch p;
                                SetSliders(p.c, cfg);
                                p.c.mode     = mode;
                                p.c.pitchHz  = f0;
                                p.c.shapeA   = (float)ia / 8.f;
                                p.c.shapeB   = (float)ib / 8.f;
                                p.c.position = (float)ip / 4.f;
                                p.c.window   = (float)iw / 4.f;
                                p.seconds    = 0.2f;
                                char buf2[160];
                                std::snprintf(buf2, sizeof buf2,
                                              "%s cfg%d f0=%g A=%.2f B=%.2f pos=%.2f win=%.2f",
                                              mode ? "shepard" : "cluster", cfg, f0,
                                              p.c.shapeA, p.c.shapeB, p.c.position,
                                              p.c.window);
                                p.name = buf2;
                                sweep.push_back(p);
                            }
                    }

    std::mt19937                          rng(20260803);
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    for (int i = 0; i < 600; ++i)
    {
        Patch p;
        for (int b = 0; b < foxtail::kNumBands; ++b)
        {
            p.c.bandGain[b] = uni(rng);
            p.c.bandShape[b] = uni(rng);
        }
        p.c.tilt     = uni(rng);
        p.c.bandShift = uni(rng);
        p.c.spread   = uni(rng);
        p.c.inharm   = uni(rng);
        p.c.mode     = uni(rng) < 0.5f ? 0 : 1;
        p.c.pitchHz  = 30.f * std::exp2(uni(rng) * 6.f);
        p.c.position = uni(rng);
        p.c.window   = uni(rng);
        p.c.shapeA   = uni(rng);
        p.c.shapeB   = uni(rng);
        p.c.parity   = uni(rng) < 0.5f ? 0.f : 1.f;
        p.seconds    = 0.2f;
        char buf2[160];
        std::snprintf(buf2, sizeof buf2, "rand#%d %s A=%.2f B=%.2f", i,
                      p.c.mode ? "shepard" : "cluster", p.c.shapeA, p.c.shapeB);
        p.name = buf2;
        sweep.push_back(p);
    }

    const Worst worst = RenderWorst(sweep);

    char buf[224];
    std::snprintf(buf, sizeof buf, "%zu patches, worst peak %.3f (%s)", sweep.size(),
                  worst.peak, sweep[worst.idx].name.c_str());
    Check(worst.peak < knee, buf);

    // --- 2b. High density, with a window long enough to see the beat ----------
    // A collapsed cluster's members sit (1-d)*f0 apart, so at d = 0.995 and
    // f0 = 55 they beat with a ~3.6 s period. The 0.2 s sweep above measures a
    // fraction of one cycle and reports a peak that is metres below the real
    // one, which is why this corner reads clean while hardware clips.
    std::printf("\nhigh density, several beat periods:\n");
    std::vector<Patch> longRun;
    {
        const float f0s2[] = {55.f, 220.f};
        const float Bs[]   = {0.90f, 0.95f, 0.99f, 1.00f};
        for (float f0 : f0s2)
            for (float B : Bs)
                for (int ia = 4; ia <= 8; ++ia)
                    for (int ip = 0; ip <= 2; ++ip)
                        for (int iw = 2; iw <= 4; ++iw)
                        {
                            Patch p;
                            SetSliders(p.c, 2); // whole bank in one channel
                            p.c.mode     = foxtail::kModeCluster;
                            p.c.pitchHz  = f0;
                            p.c.shapeA   = (float)ia / 8.f;
                            p.c.shapeB   = B;
                            p.c.position = (float)ip / 4.f;
                            p.c.window   = (float)iw / 4.f;
                            // The beat period goes as 1/f0, so the window that
                            // covers it does too.
                            p.seconds    = f0 <= 100.f ? 8.f : 3.f;
                            char b2[160];
                            std::snprintf(b2, sizeof b2,
                                          "f0=%g A=%.2f B=%.2f pos=%.2f win=%.2f", f0,
                                          p.c.shapeA, B, p.c.position, p.c.window);
                            p.name = b2;
                            longRun.push_back(p);
                        }
    }
    const Worst wLong = RenderWorst(longRun);
    std::snprintf(buf, sizeof buf, "worst peak %.3f, crest %.1f (%s)", wLong.peak,
                  wLong.peak / wLong.rms, longRun[wLong.idx].name.c_str());
    Check(wLong.peak < knee, buf);

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

    // --- 3b. Cluster knob at centre must be a true bypass ---------------------
    // In the deadzone every cluster is one partial and nothing shifts, so the
    // density knob must have NO effect — same peak at density 0 and 1. The
    // first compensation formula failed this (span m instead of m-1: a phantom
    // -1.6 dB cut at density 1, jittering on knob noise).
    {
        foxtail::Controls c;
        SetSliders(c, 0);
        c.mode    = foxtail::kModeCluster;
        c.pitchHz = 220.f;
        c.window  = 1.f;
        c.shapeA  = 0.5f;
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
        // 0.4806 measured with FOXTAIL_CLUSTER_NORM=0; at density 0 the
        // compensation is boost=1 and only FastRSqrt(1) = 0.9983 remains.
        // Re-measure whenever kHeadroom, kDarkBoost, the pan layout or the tilt
        // changes -- all of them move it, and it scales linearly with kHeadroom.
        // cfg 1 is ORBIT and dark, so it carries kDarkBoost too.
        std::snprintf(buf, sizeof buf, "density 0 untouched (peak %.4f, raw 0.4806)", m.peak);
        Check(std::fabs(m.peak - 0.4806f * 0.9983f) < 0.005f, buf);
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