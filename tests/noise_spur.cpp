// Offline reproduction of the hardware "extra harmonics near 8 kHz in
// Cluster" report. The emulator has no ADC noise, which is why it never shows
// hardware-only artifacts; this test injects per-block noise on the controls
// (the firmware reads every control once per 5-frame block) and FFTs the
// output, reporting any spectral component that is not one of the engine's
// own partials.
//
// Variants cover the suspects separately: mode (cluster/shepard), noise on
// sliders (never conditioned in FoxTail.cpp), noise on the shaper knobs
// (conditioned/frozen on hardware — raw here to test the conditioning's
// value), and V/oct jitter (never conditioned).

// -DFOXTAIL_HDR='"<path>"' selects the engine version under test, so the same
// file can be compiled against HEAD's header for A/B comparison.
#ifndef FOXTAIL_HDR
#define FOXTAIL_HDR "../foxtail_dsp.h"
#endif
#include FOXTAIL_HDR

#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr float  kSampleRate = 48000.f;
constexpr size_t kBlock      = 5;
constexpr int    kFftLog2    = 18; // 262144 samples = 5.46 s, 0.18 Hz bins
constexpr size_t kFft        = (size_t)1 << kFftLog2;

void Fft(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * M_PI / (double)len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1.0);
            for (size_t j = 0; j < len / 2; ++j)
            {
                const auto u = a[i + j];
                const auto v = a[i + j + len / 2] * w;
                a[i + j]           = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

struct Scenario
{
    std::string name;
    int   mode;
    int   band;         // which slider is up
    float sliderNoise;  // sigma per block
    float knobNoise;    // sigma per block on position/window/shapeA/shapeB
    float voctNoise;    // sigma per block, octaves
};

void Run(const Scenario& sc)
{
    static foxtail::FoxTailOsc osc;
    osc.Init(kSampleRate);

    foxtail::Controls base;
    for (int b = 0; b < foxtail::kNumBands; ++b) base.bandGain[b] = 0.f;
    base.bandGain[sc.band] = 1.f;
    base.mode     = sc.mode;
    base.pitchHz  = 220.f;
    // Realistic parked-knob offsets: hardware pots do not read exactly 0/1.
    base.position = 0.02f;
    base.window   = 0.98f;
    base.shapeA   = 0.52f; // knob 4 is centre-neutral in both modes
    base.shapeB   = 0.03f;
    base.master   = 0.7f;

    std::mt19937 rng(99);
    std::normal_distribution<float> n01(0.f, 1.f);

    std::vector<float> mono(kFft);
    float outL[kBlock], outR[kBlock];

    // settle
    for (int i = 0; i < 4000; ++i) osc.Process(base, outL, outR, kBlock);

    for (size_t i = 0; i < kFft; i += kBlock)
    {
        foxtail::Controls c = base;
        c.bandGain[sc.band] += sc.sliderNoise * n01(rng);
        c.position += sc.knobNoise * n01(rng);
        c.window   += sc.knobNoise * n01(rng);
        c.shapeA   += sc.knobNoise * n01(rng);
        c.shapeB   += sc.knobNoise * n01(rng);
        c.pitchCv   = sc.voctNoise * n01(rng);
        osc.Process(c, outL, outR, kBlock);
        for (size_t j = 0; j < kBlock && i + j < kFft; ++j)
            mono[i + j] = 0.5f * (outL[j] + outR[j]);
    }

    std::vector<std::complex<double>> spec(kFft);
    for (size_t i = 0; i < kFft; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / kFft);
        spec[i] = mono[i] * w;
    }
    Fft(spec);

    // A bin is "explained" if it is near a partial (or near DC). Partials
    // wobble with the injected noise, so allow a generous +-8 bins.
    const double binHz = kSampleRate / (double)kFft;
    std::vector<bool> explained(kFft / 2, false);
    for (size_t i = 0; i < 40; ++i) explained[i] = true;
    for (int k = 0; k < foxtail::kNumPartials; ++k)
    {
        const double f = osc.PartialFreq(k);
        const long   c = (long)(f / binHz + 0.5);
        for (long d = -8; d <= 8; ++d)
            if (c + d >= 0 && c + d < (long)(kFft / 2)) explained[c + d] = true;
    }

    // Top spurs: local maxima among unexplained bins.
    struct Spur { double f, db; };
    std::vector<Spur> spurs;
    const double ref = (double)kFft / 4.0; // Hann coherent gain 0.5, full scale
    for (size_t i = 40; i + 1 < kFft / 2; ++i)
    {
        if (explained[i]) continue;
        const double m  = std::abs(spec[i]);
        const double ml = std::abs(spec[i - 1]);
        const double mr = std::abs(spec[i + 1]);
        if (m < ml || m < mr) continue;
        const double db = 20.0 * std::log10(m / ref + 1e-30);
        if (db > -110.0) spurs.push_back({i * binHz, db});
    }
    std::sort(spurs.begin(), spurs.end(),
              [](const Spur& a, const Spur& b) { return a.db > b.db; });

    std::printf("%-46s", sc.name.c_str());
    if (spurs.empty())
    {
        std::printf("  clean (no spur above -110 dBFS)\n");
        return;
    }
    std::printf("  worst spurs:");
    for (size_t i = 0; i < 4 && i < spurs.size(); ++i)
        std::printf("  %.0f Hz %.0f dB", spurs[i].f, spurs[i].db);
    std::printf("\n");
}

} // namespace

int main()
{
    const float sl = 0.001f; // ~2 LSB of a 12-bit read, per block
    const float kn = 0.001f;
    const float vo = 0.003f; // octaves; ~5 cents rms

    const Scenario scenarios[] = {
        {"cluster band3, no noise",        0, 2, 0.f, 0.f, 0.f},
        {"cluster band3, slider noise",    0, 2, sl,  0.f, 0.f},
        {"cluster band3, knob noise",      0, 2, 0.f, kn,  0.f},
        {"cluster band3, voct noise",      0, 2, 0.f, 0.f, vo},
        {"cluster band3, all noise",       0, 2, sl,  kn,  vo},
        {"cluster band6, all noise",       0, 5, sl,  kn,  vo},
        {"shepard band3, all noise",       1, 2, sl,  kn,  vo},
        {"shepard band6, all noise",       1, 5, sl,  kn,  vo},
    };
    for (const Scenario& sc : scenarios) Run(sc);
    return 0;
}