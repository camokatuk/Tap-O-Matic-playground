#pragma once
//
// Fox Tail — additive sine oscillator DSP.
//
// This header is the "seam": pure DSP, no libDaisy, no hardware types. It is
// included by BOTH the firmware (FoxTail.cpp fills Controls from the Daisy
// hardware) and the desktop emulator (emulator/ fills Controls from the GUI),
// so the exact same synthesis runs in both places. Must stay C++14-safe
// (firmware builds -std=gnu++14): no std::clamp, no std::optional.
//
// Units are normalized, matching what time_machine_hardware.cpp already hands
// the DSP: knob/slider values are 0..1, CV values are -1..+1. No volts here.
//
// The design and the reasoning behind every constant here live in
// docs/claude/control-maps.md. Short version:
//
//   * A bank of kNumPartials sine partials, each with its own fixed-point phase
//     accumulator reading one shared sine table. NOT IFFT-to-wavetable — that
//     retunes only by rebuilding the table and is bursty, which the noise budget
//     (docs/claude/whinebug.md) forbids.
//   * 9 sliders are breakpoints of a piecewise-linear spectral envelope in
//     log-frequency, over 9 geometric bands spanning the whole bank (a full
//     octave each at 512 partials, narrower below that). 9 pots pan each band.
//   * One shaper, two modes: Cluster and Shepard. Both retune partials inside a
//     movable window, once per block.
//   * Constant work per callback: every partial is always rendered, silent ones
//     included. No sub-rate update tricks — whinebug rule 2 makes any periodic
//     structure slower than the callback rate an audible comb.
//
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace foxtail {

// The one knob to turn for CPU (see control-maps.md). The 9 bands rescale to
// whatever this is — they are geometric, not hard-wired to octaves — so all 9
// sliders stay live at any count. Band width is kNumPartials^(1/9): a full
// octave at 512, 0.89 at 256, 0.78 at 128. The 1/sqrt(r) tilt keeps power equal
// per band for *any* geometric banding, so nothing else has to change.
//
// MEASURED ON HARDWARE (-O3, block size 5): 64 partials ~55% CPU, 128 runs
// cleanly, 256 overruns. Most of that 55% is *fixed* per-callback cost, not the
// partial bank — see control-maps.md. 128 is the shipping value until the fixed
// overhead comes down.
static constexpr int kNumPartials = 128;

static constexpr int kNumBands = 9; // 9 sliders / 9 pots / 9 LEDs
static constexpr int kSineBits = 11;
static constexpr int kSineSize = 1 << kSineBits; // 2048 entries, 8 KB
static constexpr float kTwoPi  = 6.283185307179586f;

enum ShaperMode
{
    kModeCluster = 0,
    kModeShepard = 1,
};

// Normalized control snapshot, filled once per audio block by the host
// (firmware or emulator).
struct Controls
{
    // Slider b = gain of band b; pot b = its pan (0 = left, 1 = right).
    float bandGain[kNumBands] = {0.f};
    float bandPan[kNumBands]  = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
                                 0.5f, 0.5f, 0.5f, 0.5f};

    float pitchHz = 220.f; // TIME knob: fundamental in Hz
    float pitchCv = 0.f;   // V/OCT jack, in OCTAVES (0 = none)

    // Shaper. Position and window mean the same thing in both modes; only
    // shapeA/shapeB change meaning, which is what lets a screenless panel
    // survive the mode switch.
    int   mode     = kModeCluster;
    float position = 0.f; // SPREAD:   window start, exponential over 1..N
    float window   = 1.f; // FEEDBACK: window width, exponential over 1..N
    float shapeA   = 0.f; // HPF: cluster: partials per cluster (soft-step)
                          //      shepard: phi, shift toward the next partial
    float shapeB   = 0.f; // LPF: cluster: density
                          //      shepard: gain of partials inside the window

    // Switch 2. 0 = every partial, 1 = odd partials only (hollow, clarinet-ish).
    // Smoothed internally so throwing the switch doesn't step the waveform.
    float parity = 0.f;
    float master = 0.7f;
};

// ---------------------------------------------------------------------------
// Small fast-math helpers. All approximate on purpose: these run once per
// partial per block, so a libm call here is not affordable.
// ---------------------------------------------------------------------------

// log2 for x >= 1. Exponent comes from the float bits; the mantissa gets a cubic
// fit, good to ~0.002 — far better than we need to pick a band and a crossfade.
static inline float FastLog2(float x)
{
    uint32_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    const int e = (int)((bits >> 23) & 0xFFu) - 127;
    const uint32_t mbits = (bits & 0x007FFFFFu) | 0x3F800000u; // mantissa -> [1,2)
    float m;
    std::memcpy(&m, &mbits, sizeof(m));
    return (float)e + ((-0.34484843f * m + 2.02466578f) * m - 1.67487759f);
}

// 1/sqrt(x), Quake-style seed plus one Newton step (~0.2% error). Used for the
// spectral tilt, which is a taste curve, not a measurement.
static inline float FastRSqrt(float x)
{
    float    h = 0.5f * x;
    uint32_t i;
    std::memcpy(&i, &x, sizeof(i));
    i = 0x5F3759DFu - (i >> 1);
    float y;
    std::memcpy(&y, &i, sizeof(y));
    return y * (1.5f - h * y * y);
}

// Branchless. A ternary compiles to vcmpe + vmrs (FP compare, then copy flags to
// the core register), which stalls the M7 pipeline; std::fminf/fmaxf give the
// FPv5 single-instruction vminnm/vmaxnm instead. This runs several times per
// partial per block, so it is worth the pedantry.
static inline float Clamp01(float x) { return std::fminf(std::fmaxf(x, 0.f), 1.f); }

// Cubic soft clip. Backstop only — the gain staging below is supposed to keep us
// out of here most of the time.
static inline float SoftClip(float x)
{
    if (x >= 1.5f) return 1.f;
    if (x <= -1.5f) return -1.f;
    return x - (4.f / 27.f) * x * x * x;
}

// ---------------------------------------------------------------------------

class FoxTailOsc
{
  public:
    // No in-class member initializers on the arrays below: that keeps the object
    // BSS-eligible so the firmware can drop the whole thing in DTCM with
    // libDaisy's DTCM_MEM_SECTION. DTCM is uncached and zero-wait-state, so every
    // sample costs the same — which is exactly the constant-current behaviour the
    // noise budget wants. The default .bss lands in cached AXI SRAM instead,
    // where refills burst. See control-maps.md.
    void Init(float sampleRate)
    {
        sampleRate_ = sampleRate;
        nyquist_    = 0.5f * sampleRate;

        for (int i = 0; i <= kSineSize; ++i) // +1 guard entry: no wrap test in the
        {                                    // interpolation, table[i+1] is safe
            sine_[i] = std::sin(kTwoPi * (float)i / (float)kSineSize);
        }

        // ~5 ms one-pole. TWO coefficients, because the two things being smoothed
        // are updated at different rates: master runs per sample, the band gains
        // run once per block in UpdateBlock. Using the per-sample coefficient at
        // block rate stretches the time constant by the block length — which made
        // gain and pan lag by ~1.8 s at a 512-frame buffer.
        smoothCoef_    = 1.f - std::exp(-1.f / (kGainSmoothSec * sampleRate_));
        smoothCoefBlk_ = smoothCoef_;
        lastFrames_    = 0;

        // Bands are geometric over the whole bank: bandPos = log2(r) * bandScale_.
        const float lgN = FastLog2((float)kNumPartials);
        bandScale_ = (float)kNumBands / (lgN > 0.f ? lgN : 1.f); // guard N=1

        // Meter reference: a band at full slider carries power
        // kHeadroom^2 * ln(kNumPartials)/kNumBands, so this makes it read ~1.0.
        meterScale_ = 1.f / (kHeadroom * std::sqrt(std::log((float)kNumPartials)
                                                   / (float)kNumBands));

        masterSmooth_ = 0.f;
        paritySmooth_ = 0.f;
        winStart_     = 1.f;
        winEnd_        = 1.f;

        for (int b = 0; b < kNumBands; ++b)
        {
            bandLSmooth_[b] = 0.f;
            bandRSmooth_[b] = 0.f;
            bandEnergy_[b]  = 0.f;
            meter_[b]       = 0.f;
        }
        // Random initial phases, always. Aligned phases make every partial peak
        // together (an impulse train): measured crest factor 6.8 and clipping at
        // full sliders, versus 3.2 dispersed. Free headroom, no timbral cost, so
        // it never deserved a panel switch.
        uint32_t seed = kPhaseSeed;
        for (int k = 0; k < kNumPartials; ++k)
        {
            phase_[k] = NextRand(seed);
            inc_[k]   = 0;
            ampL_[k]  = 0.f;
            ampR_[k]  = 0.f;
        }
    }

    void Process(const Controls& c, float* outL, float* outR, size_t frames)
    {
        // Block-rate smoothing coefficient depends on the block length. Cached,
        // so the exp() only runs when the host changes its buffer size.
        if (frames != lastFrames_)
        {
            lastFrames_ = frames;
            const float a = 1.f - std::exp(-(float)frames / (kGainSmoothSec * sampleRate_));
            smoothCoefBlk_ = a > 1.f ? 1.f : a;
        }

        UpdateBlock(c);

        for (size_t i = 0; i < frames; ++i)
        {
            masterSmooth_ += (c.master - masterSmooth_) * smoothCoef_;

            float l = 0.f;
            float r = 0.f;
            for (int k = 0; k < kNumPartials; ++k)
            {
                const uint32_t p   = phase_[k];
                const uint32_t idx = p >> (32 - kSineBits);
                const float    fr  = (float)(p & kFracMask) * kFracScale;
                const float    s   = sine_[idx] + (sine_[idx + 1] - sine_[idx]) * fr;
                l += s * ampL_[k];
                r += s * ampR_[k];
                phase_[k] = p + inc_[k]; // uint32 wraps for free
            }

            outL[i] = SoftClip(l * masterSmooth_);
            outR[i] = SoftClip(r * masterSmooth_);
        }
    }

    // Per-band level for the 9 panel LEDs (0..1).
    float Meter(int band) const { return meter_[band]; }

    // --- Introspection, for the emulator's partial viewer ----------------------
    // The firmware never calls these, so they cost it nothing. Reading the real
    // engine state (rather than recomputing the layout in the UI) is the point:
    // the viewer cannot drift from what is actually being rendered.
    float PartialFreq(int k) const
    {
        return (float)inc_[k] * (sampleRate_ * (1.f / 4294967296.f));
    }
    float PartialAmpL(int k) const { return ampL_[k]; }
    float PartialAmpR(int k) const { return ampR_[k]; }
    // Shaper window, in partial-index space (1 = fundamental).
    float WindowStart() const { return winStart_; }
    float WindowEnd() const { return winEnd_; }

  private:
    static constexpr float kGainSmoothSec = 0.005f;

    // Tuned against the bench harness rather than derived: all sliders up with
    // random phases measures a crest factor of ~3.3 (vs ~9.0 with every partial
    // starting at phase 0 — that 8.8 dB is what phase dispersion buys, and it is
    // why it is on by default). At 1/5 that puts the normal worst case near
    // -15 dBFS RMS with peaks around 0.8, leaving the soft clip to catch only
    // Cluster at high density, where collapsed partials sum coherently.
    static constexpr float kHeadroom = 1.f / 5.f;

    // Partials fade out over the top 5% of the band rather than switching off:
    // a partial blinking off as it crosses Nyquist is a click.
    static constexpr float kNyqFadeFrac = 0.05f;

    static constexpr uint32_t kFracMask  = (1u << (32 - kSineBits)) - 1u;
    static constexpr float    kFracScale = 1.f / (float)(1u << (32 - kSineBits));

    static constexpr uint32_t kPhaseSeed = 0x9E3779B9u;

    static inline uint32_t NextRand(uint32_t& s)
    {
        s = s * 1664525u + 1013904223u;
        return s;
    }

    // Cluster: every partial inside the window slides toward the first partial of
    // its own cluster. At density 1 a cluster collapses onto one frequency; just
    // below 1 the members beat against each other, which is the chorus zone.
    // Density near 1/4, 1/2 and 1 lands on spectra that imply a subharmonic
    // (e.g. M=2, d=0.5 gives 1, 1.5, 3, 3.5 ... = multiples of f0/2).
    static inline float ClusterRatio(float idx, float p, float m, float invM, float d)
    {
        const float c = std::floor((idx - p) * invM);
        const float s = p + c * m; // first partial of this cluster
        return idx + d * (s - idx);
    }

    void UpdateBlock(const Controls& c)
    {
        const float f0      = c.pitchHz * std::exp2(c.pitchCv);
        const float hzToInc = 4294967296.f / sampleRate_; // 2^32 / sr
        const float nyqFade = 1.f / (kNyqFadeFrac * nyquist_);

        // Fold gain and pan into per-band L/R gains, then smooth those. Equal
        // power: L^2 + R^2 = 1. Interpolating already-equal-power endpoints per
        // partial is not exactly equal power, but the error is inaudible and it
        // saves two transcendentals per partial.
        for (int b = 0; b < kNumBands; ++b)
        {
            const float g  = Clamp01(c.bandGain[b]);
            const float pn = Clamp01(c.bandPan[b]);
            const float tl = g * std::sqrt(1.f - pn);
            const float tr = g * std::sqrt(pn);
            bandLSmooth_[b] += (tl - bandLSmooth_[b]) * smoothCoefBlk_;
            bandRSmooth_[b] += (tr - bandRSmooth_[b]) * smoothCoefBlk_;
            // Flush the one-pole tails to zero. Left alone they decay into
            // denormals, and denormal arithmetic on the M7 is slow enough to
            // show up as a transient CPU spike once the whole bank is doing it.
            if (bandLSmooth_[b] < 1e-20f && bandLSmooth_[b] > -1e-20f) bandLSmooth_[b] = 0.f;
            if (bandRSmooth_[b] < 1e-20f && bandRSmooth_[b] > -1e-20f) bandRSmooth_[b] = 0.f;
            bandEnergy_[b] = 0.f;
        }

        paritySmooth_ += (Clamp01(c.parity) - paritySmooth_) * smoothCoefBlk_;

        // Window, in partial-index space, exponential so the low partials (where
        // the ear cares) get most of the knob travel.
        const float nf       = (float)kNumPartials;
        const float logN     = FastLog2(nf);
        const float winStart = std::exp2(Clamp01(c.position) * logN);
        const float winWidth = std::exp2(Clamp01(c.window) * logN);
        const float winEnd   = winStart + winWidth;
        winStart_ = winStart; // published for the emulator's partial viewer
        winEnd_   = winEnd;

        // Mode params.
        const bool  shepard   = (c.mode == kModeShepard);
        const float phi       = Clamp01(c.shapeA);
        const float winGain   = shepard ? Clamp01(c.shapeB) : 1.f;
        const float density   = Clamp01(c.shapeB);
        // Partials per cluster: soft-step 1..64. The integer part sets the
        // grouping and the fraction crossfades between the two groupings, so the
        // control keeps doing something between its steps.
        const float mF   = std::exp2(Clamp01(c.shapeA) * 6.f);
        const float mLo  = std::floor(mF);
        const float mFrac = mF - mLo;
        const float mHi  = mLo + 1.f;
        const float invLo = 1.f / mLo;
        const float invHi = 1.f / mHi;

        // Window edges are tapered, not hard. A boolean edge means a partial
        // crossing it jumps frequency instantly, so sweeping SPREAD or FEEDBACK
        // drags partials across one after another and you hear a burst of clicks
        // — the "scratchy knob". The taper blends each partial's shift in and out
        // instead, at the cost of one multiply.
        const float edge    = winWidth * 0.1f > 1.f ? winWidth * 0.1f : 1.f;
        const float invEdge = 1.f / edge;

        for (int k = 0; k < kNumPartials; ++k)
        {
            const float idx = (float)(k + 1); // partial 1 is the fundamental

            // Trapezoid with smoothstep shoulders: 0 outside, 1 well inside.
            float wIn  = Clamp01((idx - winStart) * invEdge);
            float wOut = Clamp01((winEnd - idx) * invEdge);
            wIn        = wIn * wIn * (3.f - 2.f * wIn);
            wOut       = wOut * wOut * (3.f - 2.f * wOut);
            const float w = wIn * wOut;

            float shifted = idx;
            if (shepard)
            {
                shifted = idx + phi;
            }
            else
            {
                const float rLo = ClusterRatio(idx, winStart, mLo, invLo, density);
                const float rHi = ClusterRatio(idx, winStart, mHi, invHi, density);
                shifted         = rLo + (rHi - rLo) * mFrac;
            }

            const float ratio = idx + w * (shifted - idx);
            const float wGain = shepard ? (1.f + w * (winGain - 1.f)) : 1.f;

            const float freq = ratio * f0;

            // Clamp to Nyquist before the integer conversion. Partial 512 at
            // f0 = 440 Hz wants 225 kHz, whose increment overflows the
            // float->integer conversion outright (UB). Those partials are muted
            // by the fade below, so pinning their increment costs nothing.
            const float incHz = std::fminf(freq, nyquist_);
            inc_[k]           = (uint32_t)(incHz * hzToInc);

            // The spectral envelope is sampled at the SHIFTED frequency, not at
            // the partial index. That is what makes Shepard seamless: at phi = 1
            // partial k sits where partial k+1 was, so if amplitude depends only
            // on frequency the ensemble before and after the wrap is identical
            // and nothing clicks. It also means you draw the Shepard envelope
            // with the sliders.
            const float lg = FastLog2(ratio) * bandScale_; // 0 .. 9 across the bank
            int         b  = (int)lg;
            if (b < 0) b = 0;
            if (b > kNumBands - 1) b = kNumBands - 1;
            int b1 = b + 1;
            if (b1 > kNumBands - 1) b1 = kNumBands - 1;
            const float t = Clamp01(lg - (float)b);

            const float gl = bandLSmooth_[b] + (bandLSmooth_[b1] - bandLSmooth_[b]) * t;
            const float gr = bandRSmooth_[b] + (bandRSmooth_[b1] - bandRSmooth_[b]) * t;

            // 1/sqrt(ratio) is a -3 dB/octave tilt, which makes every octave band
            // carry equal power (band b holds 2^b partials at amplitude 2^-b/2).
            // So one multiply does the job of both the spectral tilt and the
            // per-band 1/sqrt(M) normalisation, and all sliders up = a pink-ish
            // spectrum rather than something top-heavy.
            const float tilt = FastRSqrt(ratio);

            // Anti-alias fade, branch-free.
            const float fade = Clamp01((nyquist_ - freq) * nyqFade);

            // Parity: fade the even-numbered partials out. Partial number is
            // k+1, so k odd == an even partial.
            const float par = (k & 1) ? (1.f - paritySmooth_) : 1.f;

            const float a = tilt * fade * wGain * par * kHeadroom;
            float       al = gl * a;
            float       ar = gr * a;
            // Same denormal guard as above, for the same reason.
            al = al < 1e-20f ? 0.f : al;
            ar = ar < 1e-20f ? 0.f : ar;
            ampL_[k] = al;
            ampR_[k] = ar;

            // Accumulate *audible* energy per band for the LEDs: this includes
            // the Nyquist fade and the shaper's window gain, so a band whose
            // partials have run off the top of the spectrum goes dark instead of
            // just reporting its slider position back to you.
            bandEnergy_[b] += al * al + ar * ar;
        }

        for (int b = 0; b < kNumBands; ++b)
        {
            meter_[b] = std::fminf(std::sqrt(bandEnergy_[b]) * meterScale_, 1.f);
        }
    }

    float sampleRate_;
    float nyquist_;
    float  smoothCoef_;    // per sample  (master)
    float  smoothCoefBlk_; // per block   (band gains/pans)
    size_t lastFrames_;
    float  masterSmooth_;
    float  paritySmooth_;

    float winStart_;
    float winEnd_;
    float bandScale_;  // log2(ratio) -> band position
    float meterScale_; // band energy -> 0..1 for the LEDs
    float bandEnergy_[kNumBands];

    float bandLSmooth_[kNumBands];
    float bandRSmooth_[kNumBands];
    float meter_[kNumBands];

    uint32_t phase_[kNumPartials];
    uint32_t inc_[kNumPartials];
    float    ampL_[kNumPartials];
    float    ampR_[kNumPartials];

    float sine_[kSineSize + 1];
};

} // namespace foxtail