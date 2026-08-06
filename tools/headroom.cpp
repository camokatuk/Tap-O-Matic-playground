// Headroom report: how much of the budget each class of patch actually uses.
//
// clip_guard asserts the top — nothing may reach the clip. This asks the other
// question: how far BELOW the knee everything sits, and how unevenly. A fixed
// headroom budget is sized against the loudest patch the panel can reach, so
// every quieter patch pays for one nobody plays; this measures that bill.
//
// A tuning tool, not a test: the numbers are design input and what counts as
// too quiet is a taste call. tests/run.sh does not build it — tests are for
// regressions. Run it on demand with ./tools/headroom.sh.

#include "../tests/harness.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using fxtest::Db;
using fxtest::Meas;
using fxtest::Patch;

namespace {

// N sliders up, spread evenly over the eight bands, the rest at zero. The panel
// class that matters: a real patch picks a few bands, not all of them.
void SetBands(foxtail::Controls& c, int n)
{
    for (int b = 0; b < foxtail::kNumBands; ++b) c.bandGain[b] = 0.f;
    for (int i = 0; i < n; ++i)
        c.bandGain[(i * foxtail::kNumBands) / n] = 1.f;
}

struct Class
{
    const char* name;
    int         bands;   // 0 = random patch class
    float       spread;
    float       tilt;
};

const Class kClasses[] = {
    {"8 bands up, dark",   8, 0.f, 0.f},
    {"8 bands up, bright", 8, 0.f, 1.f},
    {"8 bands up, spread", 8, 1.f, 0.f},
    {"4 bands up",         4, 0.f, 0.f},
    {"3 bands up",         3, 0.f, 0.f},
    {"2 bands up",         2, 0.f, 0.f},
    {"1 band up",          1, 0.f, 0.f},
    {"random",             0, 0.f, 0.f},
};

// Below this a patch is not quiet, it is off: nothing a player would call a
// sound, and a knob position that reads as a broken module.
constexpr float kDeadDb = -80.f;

float Pct(std::vector<float> v, float p)
{
    if (v.empty()) return 0.f;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)(p * (float)(v.size() - 1) + 0.5f);
    return v[i];
}

} // namespace

int main()
{
    const float knee = foxtail::kClipKnee;

    std::vector<Patch> patches;
    std::vector<int>   cls; // class index per patch

    const float f0s[] = {55.f, 220.f, 880.f};

    std::mt19937                          rng(20260806);
    std::uniform_real_distribution<float> uni(0.f, 1.f);

    for (int ci = 0; ci < (int)(sizeof kClasses / sizeof kClasses[0]); ++ci)
    {
        const Class& k = kClasses[ci];
        if (k.bands == 0)
        {
            for (int i = 0; i < 1200; ++i)
            {
                Patch p;
                for (int b = 0; b < foxtail::kNumBands; ++b)
                {
                    p.c.bandGain[b]  = uni(rng);
                    p.c.bandShape[b] = uni(rng);
                }
                p.c.tilt      = uni(rng);
                p.c.bandShift = uni(rng);
                p.c.spread    = uni(rng);
                p.c.inharm    = uni(rng);
                p.c.mode      = uni(rng) < 0.5f ? 0 : 1;
                p.c.pitchHz   = 30.f * std::exp2(uni(rng) * 6.f);
                p.c.position  = uni(rng);
                p.c.window    = uni(rng);
                p.c.shapeA    = uni(rng);
                p.c.shapeB    = uni(rng);
                p.c.parity    = uni(rng) < 0.5f ? 0.f : 1.f;
                p.seconds     = 0.2f;
                char buf[160];
                std::snprintf(buf, sizeof buf, "rand#%d %s A=%.2f B=%.2f f0=%.0f", i,
                              p.c.mode ? "shepard" : "cluster", p.c.shapeA, p.c.shapeB,
                              p.c.pitchHz);
                p.name = buf;
                patches.push_back(p);
                cls.push_back(ci);
            }
            continue;
        }

        for (int mode = 0; mode < 2; ++mode)
            for (float f0 : f0s)
                for (int ia = 0; ia <= 4; ++ia)
                    for (int ib = 0; ib <= 4; ++ib)
                        for (int ip = 0; ip <= 2; ++ip)
                            for (int iw = 0; iw <= 2; ++iw)
                            {
                                Patch p;
                                SetBands(p.c, k.bands);
                                p.c.spread   = k.spread;
                                p.c.tilt     = k.tilt;
                                p.c.mode     = mode;
                                p.c.pitchHz  = f0;
                                p.c.shapeA   = (float)ia / 4.f;
                                p.c.shapeB   = (float)ib / 4.f;
                                p.c.position = (float)ip / 2.f;
                                p.c.window   = (float)iw / 2.f;
                                p.seconds    = 0.2f;
                                char buf[192];
                                std::snprintf(buf, sizeof buf,
                                              "%s %s f0=%g A=%.2f B=%.2f pos=%.1f win=%.1f",
                                              mode ? "shepard" : "cluster", k.name, f0,
                                              p.c.shapeA, p.c.shapeB, p.c.position,
                                              p.c.window);
                                p.name = buf;
                                patches.push_back(p);
                                cls.push_back(ci);
                            }
    }

    const std::vector<Meas> m = fxtest::RenderAll(patches);

    std::printf("headroom report — knee %.2f, master %.2f, block %zu, %zu patches\n\n",
                knee, fxtest::kMaster, fxtest::kBlock, patches.size());

    std::printf("%-20s %7s  %8s %8s  %8s %6s  %8s %5s\n", "class", "patches", "peak max",
                "peak p50", "rms p50", "crest", "to knee", "dead");
    for (int ci = 0; ci < (int)(sizeof kClasses / sizeof kClasses[0]); ++ci)
    {
        std::vector<float> peaks, rmss, crests;
        int                dead = 0;
        for (size_t i = 0; i < patches.size(); ++i)
            if (cls[i] == ci)
            {
                peaks.push_back(m[i].peak);
                rmss.push_back(m[i].rms);
                if (Db(m[i].rms) < kDeadDb)
                    ++dead;
                else
                    crests.push_back(Db(m[i].peak) - Db(m[i].rms));
            }
        if (peaks.empty()) continue;
        const float pmax = Pct(peaks, 1.f);
        // Per-patch crest, at the loud end: the budget is sized against the peak
        // but the ear pays attention to the RMS, so the gap between them is what
        // a headroom scheme actually has to buy back.
        std::printf("%-20s %7zu  %8.3f %8.3f  %7.1f  %5.1f  %6.1f dB %5d\n",
                    kClasses[ci].name, peaks.size(), pmax, Pct(peaks, 0.5f),
                    Db(Pct(rmss, 0.5f)), Pct(crests, 0.95f), Db(knee) - Db(pmax), dead);
    }

    // The global picture: what the budget is sized against, and what it costs.
    std::vector<float> allPeaks, allRms;
    for (const Meas& x : m)
    {
        allPeaks.push_back(x.peak);
        allRms.push_back(x.rms);
    }
    const float pmax = Pct(allPeaks, 1.f);
    size_t      worst = 0, quiet = 0;
    for (size_t i = 0; i < m.size(); ++i)
    {
        if (m[i].peak > m[worst].peak) worst = i;
        if (m[i].rms < m[quiet].rms) quiet = i;
    }

    std::printf("\nloudest   %.3f peak, %.1f dBFS rms  %s\n", m[worst].peak,
                Db(m[worst].rms), patches[worst].name.c_str());
    std::printf("quietest  %.3f peak, %.1f dBFS rms  %s\n", m[quiet].peak,
                Db(m[quiet].rms), patches[quiet].name.c_str());

    // Silent patches are the other end of the same question: a control position
    // that produces nothing is a hole in the panel, not a quiet setting.
    int dead = 0;
    for (const Meas& x : m)
        if (Db(x.rms) < kDeadDb) ++dead;
    if (dead)
    {
        std::printf("\n%d patches below %.0f dBFS (silent). First few:\n", dead, kDeadDb);
        int shown = 0;
        for (size_t i = 0; i < m.size() && shown < 5; ++i)
            if (Db(m[i].rms) < kDeadDb)
            {
                std::printf("  %s\n", patches[i].name.c_str());
                ++shown;
            }
    }

    // Is the ceiling a spike or a plateau? A budget set by a handful of patches
    // can be bought back by taming them; one set by a broad shoulder cannot.
    std::printf("\npeak distribution:  p50 %.3f   p90 %.3f   p99 %.3f   p999 %.3f   max %.3f\n",
                Pct(allPeaks, 0.5f), Pct(allPeaks, 0.9f), Pct(allPeaks, 0.99f),
                Pct(allPeaks, 0.999f), pmax);
    std::printf("  taming the top 0.1%% would win %.1f dB, the top 1%% %.1f dB,"
                " the top 10%% %.1f dB\n",
                Db(pmax) - Db(Pct(allPeaks, 0.999f)), Db(pmax) - Db(Pct(allPeaks, 0.99f)),
                Db(pmax) - Db(Pct(allPeaks, 0.9f)));

    // The loudest patches, to see whether they share a control signature.
    std::vector<size_t> order(m.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return m[a].peak > m[b].peak; });
    std::printf("\nloudest 15 (peak, crest, patch):\n");
    for (int i = 0; i < 15; ++i)
    {
        const size_t j = order[(size_t)i];
        std::printf("  %.3f  %4.1f dB  %s\n", m[j].peak, Db(m[j].peak) - Db(m[j].rms),
                    patches[j].name.c_str());
    }

    std::printf("\nunused headroom at the loudest patch: %.1f dB\n", Db(knee) - Db(pmax));
    std::printf("rms spread p05..p95: %.1f dB (p50 %.1f dBFS)\n",
                Db(Pct(allRms, 0.95f)) - Db(Pct(allRms, 0.05f)), Db(Pct(allRms, 0.5f)));
    std::printf("a p50 patch sits %.1f dB under the loudest one.\n",
                Db(pmax) - Db(Pct(allPeaks, 0.5f)));
    return 0;
}
