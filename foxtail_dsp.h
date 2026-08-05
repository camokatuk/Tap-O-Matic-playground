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
//   * Band sliders are breakpoints of a piecewise-linear spectral envelope in
//     log-frequency over geometric bands spanning the whole bank, and above
//     half their travel they also fill their band in. Slider 1 is
//     inharmonicity; pot 1 is spectral shift, pot 2 fine tune, pot 3 stereo
//     spread, pots 4-9 each band's fill order. GATE picks the spectral tilt.
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

// Defeats the Cluster collapse compensation. Exists so tests/clip_guard.cpp can
// A/B the same patches against an uncompensated engine; never set it to 0 in a
// firmware build.
#ifndef FOXTAIL_CLUSTER_NORM
#define FOXTAIL_CLUSTER_NORM 1
#endif

namespace foxtail {

// The one CPU lever. Bands are geometric (width kNumPartials^(1/kNumBands)),
// so all band sliders stay live at any count, and the 1/sqrt(r) tilt keeps
// power equal per band for any geometric banding.
//
// MEASURED (-O3, block 5, H750): the budget must fit the WORST-CASE mode,
// which is Cluster (~0.52%/partial over ~31% fixed, vs Shepard's ~0.43%).
// At 96: Shepard ~72%, Cluster ~80%. At 128 Cluster overruns (>100%), and
// overload does not just glitch audio — it starves the ADC, USB and LEDs.
// Keep real headroom; 96 is the sweet spot. If more partials are ever wanted,
// optimise Cluster's per-partial math first (incremental cluster-start instead
// of floor()), not the render loop.
static constexpr int kNumPartials = 96;

// 8 bands, not 9: slider 1 and pot 1 are spent on inharmonicity and fine
// tune, leaving sliders/pots 2..9 for the bands. 8 bands also happens to be
// exact octaves if we ever reach 256 partials (bands == log2(partials)).
static constexpr int kNumBands = 8;
// No interpolation in the render loop: a 16384-entry table costs 64 KB of DTCM
// (we have 128 KB and were using 10) and buys back a table load, an AND, a
// uint->float convert and three FP ops per partial per sample. Phase truncation
// to 14 bits puts the worst spur near -84 dBc per partial, which is far below
// this board's own noise floor.
// Pan-slot assignment: row n-1 sends partial k (mod 8) to one of n positions
// across the stereo field. Searched, not derived — the obvious closed forms
// collapse for some n (3k mod n loses every position but one whenever 3 divides
// n). Each row holds three invariants, asserted in tests/slot_table.cpp: use all
// n positions; keep the even-k and odd-k subsets balanced around centre, so
// switch 2 in ODD ONLY cannot shove the image to one side; and put neighbouring
// partials as far apart as the first two allow. Internal linkage because C++14
// has no inline variables and this is a header.
static constexpr unsigned char kSlotPat[8][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 0, 1, 1, 0, 1},
    {0, 1, 2, 0, 2, 1, 0, 2},
    {0, 1, 3, 0, 3, 2, 0, 3},
    {0, 2, 3, 0, 4, 2, 1, 4},
    {0, 2, 4, 0, 5, 3, 1, 5},
    {0, 3, 4, 1, 5, 2, 3, 6},
    {0, 3, 5, 1, 7, 4, 2, 6},
};

// Pan-anchor configurations: the position of each of the 8 pan slots, -1 hard
// left to +1 hard right. Pot 3 morphs between adjacent rows, so its travel
// changes the image's SHAPE. Width alone was the original design and it wasted
// the knob -- scaling one fixed fan makes every intermediate setting a quieter
// version of the last, so only the two ends were worth visiting.
//
// Every row must keep kSlotPat's even-k/odd-k balance (tests/slot_table.cpp) or
// switch 2 in ODD ONLY shoves the image to one side.
//
// Nothing here reaches +-1. Hard panning is what puts half the bank in one
// channel at full gain instead of all of it at 0.707, and it is the only thing
// that moved a witness peak when this was first tried -- see archives.md.
static constexpr int kPanAnchors = 3;
static constexpr float kPanAnchor[kPanAnchors][8] = {
    // MONO
    {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    // SCATTER -- the even fan, neighbouring partials thrown far apart.
    {-1.f, -0.714286f, -0.428571f, -0.142857f, 0.142857f, 0.428571f, 0.714286f, 1.f},
    // ORBIT, frozen at phase 0. UpdateBlock always substitutes the live orbit
    // here, so these values exist to fix the anchor's shape at rest and to give
    // the parity test something to check.
    {0.f, 0.452548f, -0.8f, -0.452548f, -0.452548f, 0.f, 0.452548f, 0.8f},
};
static constexpr int kPanRotAnchor = 2;

// A mono park at the bottom, then equal thirds of what is left: mono -> scatter,
// scatter -> orbit, and above the last knot the shape stops changing and the
// travel drives MOTION instead (see kPanRotSlow). 0.92 / 3 = 0.306667.
static constexpr float kPanKnot[kPanAnchors] = {0.08f, 0.5f, 0.75f};

// ORBIT's per-slot phase offsets, in turns, and which of the two phase
// accumulators each slot rides. Not slot order: the offsets give the even-k and
// odd-k slot subsets FOUR POINTS 90 DEGREES APART EACH, so both parity sums
// cancel at every phase, not just at rest. Slot order (i/8) leaves each subset
// summing to ~1.08 and the image leans as it turns.
//
// Splitting the subsets across two accumulators is what lets them run at
// different rates without ever breaking that: each stays internally balanced
// whatever its own phase is.
static constexpr float kPanRotOfs[8] = {0.f,    0.125f, 0.75f,  0.625f,
                                        0.875f, 0.5f,   0.375f, 0.25f};
static constexpr unsigned char kPanRotSub[8] = {0, 1, 0, 1, 1, 0, 1, 0};

// Orbit radius. Below 1 on purpose: the slots sweep the field without ever
// reaching a hard pan, which keeps the peak profile flat across the whole knob.
static constexpr float kPanRotAmp = 0.8f;

// Revolutions per second at the ORBIT knot and at full CW. Squared against the
// ramp so the bottom of the range creeps.
static constexpr float kPanRotSlow = 0.04f;
static constexpr float kPanRotFast = 0.20f;

// What the odd-k subset's rate is multiplied by at full CW: reversed, and at a
// ratio that does not divide 1, so the two clouds drift through each other
// without the image ever repeating.
static constexpr float kPanRotCounter = -0.9918034f;

static constexpr int kSineBits = 14;
static constexpr int kSineSize = 1 << kSineBits;
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
    // Slider b: gain of band b over its whole travel, AND above half its fill —
    // how far the band's partials spread out from the seed. So the bottom half
    // is one partial per band at varying level (the skeleton comb) and the top
    // half fills each band in. Coupling the two is deliberate: a band with more
    // partials in it IS louder, and it keeps the envelope drawable at any level.
    float bandGain[kNumBands] = {0.f};

    // Pots 4-9: where band b fills FROM. 0 = its HIGHEST partial and downward
    // (highpass), 0.5 = its middle outward (bandpass), 1 = its lowest partial
    // and upward (lowpass). Highpass at 0 so that building a hump is a single
    // diagonal gesture: a band left of the peak wants to keep the partials
    // nearest the peak, which are its high ones, so pots rise left to right
    // exactly as the sliders do. Bands 0-1 hold two partials each and stay at 0.
    float bandShape[kNumBands] = {0.f};

    // Spectral tilt, 0 = 1/ratio (a sawtooth's -6 dB/octave, acoustic-ish),
    // 1 = 1/sqrt(ratio) (equal power per octave, bright). Continuous, so the
    // middle is a real slope between the two. Driven by GATE on the panel.
    float tilt = 0.f;

    // Pot 2: stereo image, morphing MONO -> SCATTER -> ORBIT (kPanAnchor), then
    // spending the rest of the travel on the orbit's speed and detune. The
    // number of pan positions is fixed at kSlots — the count was a pot once and
    // only changed the grain of a cloud the ear already hears as one texture.
    float spread = 0.f;

    // Pot 3: slides the whole spectral shape — band envelope AND comb — along
    // the harmonic series, without moving any partial's frequency. Centre is
    // neutral, an octave each way, CW = shape moves up. With the sliders near
    // half (the skeleton comb) this scans which harmonics are chosen.
    float bandShift = 0.5f;

    // Slider 1: inharmonicity. 0 = pure harmonic series; up bends the series
    // stiff-string style (piano at low settings, bell/metallic high). The k^2
    // law keeps low partials nearly pure at any setting on its own.
    float inharm = 0.f;
    // Pot 1: fine tune in semitones, -1..+1 (0 = in tune). Knob 1 spans 3.3
    // octaves over one sweep, still too coarse to tune by hand.
    float fineTune = 0.f;

    float pitchHz = 220.f; // knob 1: fundamental in Hz
    float pitchCv = 0.f;   // V/OCT jack, in OCTAVES (0 = none)

    // Shaper. Position and window mean the same thing in both modes; only
    // shapeA/shapeB change meaning, which is what lets a screenless panel
    // survive the mode switch.
    int   mode     = kModeCluster;
    float position = 0.f; // knob 2: window start, exponential over 1..N
    float window   = 1.f; // knob 3: window width, exponential over 1..N
    float shapeA   = 0.f; // knob 4: cluster: partials per cluster (soft-step)
                          //         shepard: phi, shift toward the next partial
    float shapeB   = 0.f; // knob 5: cluster: density
                          //         shepard: window duck (0 = unity gain)

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

// log2(x) for x >= 1, ~2e-5. FastLog2's degree-2 mantissa fit is 100x coarser
// and cannot be widened in its place — it sits in the per-partial loop.
static inline float Log2Fine(float x)
{
    uint32_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    const int      e     = (int)((bits >> 23) & 0xFFu) - 127;
    const uint32_t mbits = (bits & 0x007FFFFFu) | 0x3F800000u; // mantissa -> [1,2)
    float m;
    std::memcpy(&m, &mbits, sizeof(m));
    return (float)e + (((((0.043004958f * m - 0.402513394f) * m + 1.589474295f) * m
                         - 3.489878551f) * m + 5.047855413f) * m - 2.787926207f);
}

// ln(1+t)/t, for t >= 0. No libm: a logf call runs on every callback once the
// Cluster knob leaves rest, which is the per-callback burst the noise budget
// forbids. The series arm carries t -> 0, where any mantissa-extraction log
// loses every significant digit and the compensation below divides two of these
// at t down to 5e-5.
static inline float Log1pOverX(float t)
{
    if (t < 0.25f)
        return 1.f
               + t * (-0.5f + t * (1.f / 3.f + t * (-0.25f + t * (0.2f - t * (1.f / 6.f)))));
    return Log2Fine(1.f + t) * (0.69314718f / t);
}

// Branchless. A ternary compiles to vcmpe + vmrs (FP compare, then copy flags to
// the core register), which stalls the M7 pipeline; std::fminf/fmaxf give the
// FPv5 single-instruction vminnm/vmaxnm instead. This runs several times per
// partial per block, so it is worth the pedantry.
static inline float Clamp01(float x) { return std::fminf(std::fmaxf(x, 0.f), 1.f); }

// One-pole toward a target, denormal tail flushed. Block rate, so the flush is
// two compares per control per block, not per partial.
static inline float Smooth(float& s, float target, float k)
{
    s += (target - s) * k;
    if (s < 1e-20f && s > -1e-20f) s = 0.f;
    return s;
}

// Output clip: unity below the knee, parabolic to a hard cap at 2 - knee.
// The knee sits above the loudest normal patch (peak 0.73, all sliders up and
// hard-panned; tests/clip_sweep.cpp), so an oscillator patch passes bit-exact
// and only Cluster's beat transients get caught. The old cubic had no linear
// region and put ~-35 dB of waveshaping on everything.
static constexpr float kClipKnee = 0.85f;
static inline float SoftClip(float x)
{
    const float ax = std::fabs(x);
    if (ax <= kClipKnee) return x;
    const float s = x < 0.f ? -1.f : 1.f;
    if (ax >= 2.f - kClipKnee) return s;
    const float u = ax - kClipKnee;
    return s * (ax - u * u * (0.25f / (1.f - kClipKnee)));
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

        for (int i = 0; i < kSineSize; ++i)
            sine_[i] = std::sin(kTwoPi * (float)i / (float)kSineSize);

        // ~5 ms one-pole. TWO coefficients, because the two things being smoothed
        // are updated at different rates: master runs per sample, the band gains
        // run once per block in UpdateBlock. Using the per-sample coefficient at
        // block rate stretches the time constant by the block length — which made
        // gain and pan lag by ~1.8 s at a 512-frame buffer.
        smoothCoef_    = 1.f - std::exp(-1.f / (kGainSmoothSec * sampleRate_));
        smoothCoefBlk_ = smoothCoef_;
        lastFrames_    = 0;
        blockSec_      = 1.f / sampleRate_;
        panRotPhA_     = 0.f; // zeroed here so a re-Init renders reproducibly
        panRotPhB_     = 0.f;

        // Bands are geometric over the whole bank: bandPos = log2(r) * bandScale_.
        const float lgN = FastLog2((float)kNumPartials);
        bandScale_ = (float)kNumBands / (lgN > 0.f ? lgN : 1.f); // guard N=1

        masterSmooth_ = 0.f;
        paritySmooth_ = 0.f;
        sh_           = Shaper();
        winStart_     = 1.f;
        winEnd_        = 1.f;

        for (int i = 0; i < 2 * kSlots; ++i)
            panDelta_[i] = 0.f; // centred until the first block
        bandShift_ = 1.f;
        bpShift_   = -0.5f;
        tiltSmooth_ = 0.f;
        tiltInv_    = 1.f;
        for (int k = 0; k < kNumPartials; ++k)
            bpIdx_[k] = FastLog2((float)(k + 1)) * bandScale_;
        for (int b = 0; b < kNumBands; ++b)
        {
            bandGainSmooth_[b] = 0.f;
            bandGainStep_[b]   = 0.f;
            meter_[b]   = 0.f;
            audible_[b] = true;
            bandLoRatio_[b] = std::exp2(((float)b - 0.5f) / bandScale_);
            // Band b spans these partials. In PARTIALS, not log-frequency, so
            // the comb steps one harmonic at a time everywhere; the bands are
            // geometric, so that is 2 partials at the bottom and 32 near the top.
            bandSpanLo_[b] = std::exp2(((float)b + 0.5f) / bandScale_);
            bandSpanHi_[b] = std::exp2(((float)b + 1.5f) / bandScale_);
            bandSpanW_[b]  = bandSpanHi_[b] - bandSpanLo_[b];
            bandSeed_[b]   = bandSpanHi_[b];
            bandGate_[b]   = 1.f;
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
            blockSec_      = (float)frames / sampleRate_;
        }

        UpdateBlock(c);

        for (size_t i = 0; i < frames; ++i)
        {
            masterSmooth_ += (c.master - masterSmooth_) * smoothCoef_;

            float l = 0.f;
            float r = 0.f;
            for (int k = 0; k < kNumPartials; ++k)
            {
                const uint32_t p = phase_[k];
                const float    s = sine_[p >> (32 - kSineBits)];
                l += s * ampL_[k];
                r += s * ampR_[k];
                phase_[k] = p + inc_[k]; // uint32 wraps for free
            }

            outL[i] = SoftClip(l * masterSmooth_);
            outR[i] = SoftClip(r * masterSmooth_);
        }
    }

    // What the LEDs show: the band's own gain. Energy() is the *audible* energy
    // in that band, which is a different question — a band can be turned up and
    // still be silent because its partials have run past Nyquist. Showing energy
    // on the LEDs looked wrong because the envelope legitimately spills into
    // neighbouring bands, so one raised slider lit three lamps.
    float Meter(int band) const { return meter_[band]; }
    // True when this band still has partials below Nyquist. Derived per band
    // from the band's lower edge rather than by accumulating energy per partial
    // — that accumulation cost ~10 instructions per partial per block and this
    // is 8 comparisons per block. Approximate once the shaper moves partials
    // around, which is fine for a lamp.
    bool Audible(int band) const { return audible_[band]; }

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

    // Which band a given partial index falls in, as a continuous position
    // (0 = centre of band 0). Lets the firmware paint the window on the LEDs.
    float BandOfRatio(float r) const
    {
        return r <= 1.f ? -0.5f : FastLog2(r) * bandScale_ - 0.5f;
    }

  private:
    static constexpr float kGainSmoothSec = 0.005f;

    // Tuned against the bench harness rather than derived: all sliders up with
    // random phases measures a crest factor of ~3.3 (vs ~9.0 with every partial
    // starting at phase 0 — that 8.8 dB is what phase dispersion buys, and it is
    // why it is on by default).
    //
    // Raised from 1/5 after the tilt went to 1/r, which cost ~5 dB and left the
    // worst patch anywhere peaking 0.497 against the 0.85 knee — 4.6 dB unused.
    // 0.30 spends 3.5 dB of that and keeps the guard's contract (nothing reaches
    // the knee, worst case ~0.75). Sizing this to all-eight-sliders-up is
    // conservative to begin with: a bank with random phases sums as sqrt(N), so
    // a normal two- or three-band patch sits 6-9 dB below the case this
    // protects. Going louder still means deciding that patch may saturate --
    // which is what a soft clip is for, but it is a contract change, not a tweak.
    static constexpr float kHeadroom = 0.30f;

    // Past ~0.995 the beat periods outlast any note; exact 1.0 buys nothing.
    static constexpr float kDensityMax = 0.995f;

    // Stereo spread is quantised to kSlots pan positions across the field, built
    // once per block. The ear cannot place a single partial finely enough to
    // tell this from a continuous pan law, and the quantisation is what makes
    // the feature affordable: the equal-power sqrt pair moves out of the
    // per-partial loop (cost check: tools/loop_cost.py).
    //
    // Row n-1 assigns partial k (mod 8) to one of n pan positions. Searched, not
    // derived: the obvious closed forms collapse for some n (3k mod n loses
    // every position but one whenever 3 divides n). Each row satisfies three
    // invariants, asserted in tests/slot_table.cpp — use all n positions, keep
    // the even-k and odd-k subsets balanced around centre so switch 2 in ODD
    // ONLY cannot shove the image to one side, and put neighbouring partials as
    // far apart as that allows.
    // (The table itself is at namespace scope: C++14 has no inline variables, so
    // a constexpr member indexed at runtime would need an out-of-class
    // definition that a header cannot give.)
    static constexpr int   kSlots  = 8;
    static constexpr float kCentre = 0.70710678f;

    // Shepard-only band crossfade: linear over the top kXfadeW of a band, ending
    // exactly at the boundary so the gain is continuous across it. Narrow on
    // purpose — wide enough that a partial takes many blocks to cross at any
    // sane phi rate, narrow enough that a band still owns its own partials.
    static constexpr float kXfadeW   = 0.15f;
    static constexpr float kXfadeLo  = 1.f - kXfadeW;
    static constexpr float kXfadeInv = 1.f / kXfadeW;

    // The spectral shift pot's range, in octaves either side of neutral.
    static constexpr float kBandShiftOct = 1.f;

    // Ratio at which Shepard's top end reaches silence: one index above the last
    // partial, which is where the highest partial lands at full phi.
    static constexpr float kEndFade = (float)kNumPartials + 1.f;

    // Tilt level compensation, fitted at kNumPartials = 96. Refit if that moves:
    // g(m) = sqrt(P(0)/P(m)), P(m) = sum over the bank of the tilted amplitude
    // squared. tools/ has no generator for this; it is ten lines of Python.
    static constexpr float kTiltNorm1 = -0.5604f;
    static constexpr float kTiltNorm2 = 0.1240f;

    // Partials fade out over the top 5% of the band rather than switching off:
    // a partial blinking off as it crosses Nyquist is a click.
    static constexpr float kNyqFadeFrac = 0.05f;

    // Playable range for the effective fundamental (TIME knob x pitch CV). The
    // ceiling keeps a railed CV input (+5 V = +5 octaves) from pushing every
    // partial past Nyquist and silencing the module; 6 kHz still leaves the
    // first couple of partials audible at the extreme.
    static constexpr float kF0Min = 8.f;
    static constexpr float kF0Max = 6000.f;

    // Slider 1 at full stretches partial 128 to roughly 3x its harmonic position.
    // Sized so the effect is unmistakable: at full, partial 16 is ~33% sharp
    // and the top of the bank lands several times above its harmonic position.
    // (Smaller values only move the top partials, which is barely audible.)
    static constexpr float kInharmMax   = 2.0e-3f;

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
    // Offset from the window start to the first partial of this cluster.
    // Returns the START, not the shifted ratio: both groupings of the soft-step
    // then share one crossfade and one collapse, instead of each carrying its
    // own copy of both. rel = max(idx - p, 0), hoisted by the caller — the clamp
    // is what keeps partials below the window start off a cluster start at p - M.
    static inline float ClusterStart(float rel, float m, float invM)
    {
        return std::floor(rel * invM) * m;
    }

    // Sine of a phase in turns, wrapped by the table mask. Block rate and only
    // for pan positions, so the 14-bit table is far finer than the ear needs.
    // Callers must keep the argument non-negative; the mask is the only wrap.
    float SineTurns(float turns) const
    {
        return sine_[(int)(turns * (float)kSineSize) & (kSineSize - 1)];
    }

    void UpdateBlock(const Controls& c)
    {
        // Every control that reaches a partial's FREQUENCY is smoothed here, the
        // same way the gains are. Raw, these land on the frequencies once per
        // block, so ADC noise is random FM at the callback rate — ~2 cents per
        // LSB with the window collapsed onto one cluster, which is the crackle.
        // One-poles, NOT the backlash gate that was tried and clicked
        // (archives.md): a gate makes the noise sparse and larger instead.
        // Smoothing lives here rather than at the hardware seam so the emulator
        // gets it too — it runs this engine, and its CV sources must behave like
        // a patched jack. Pitch is deliberately NOT here: it would lag V/oct.
        const float k        = smoothCoefBlk_;
        const float position = Smooth(sh_.position, Clamp01(c.position), k);
        const float window   = Smooth(sh_.window, Clamp01(c.window), k);
        const float shapeA   = Smooth(sh_.shapeA, Clamp01(c.shapeA), k);
        const float shapeB   = Smooth(sh_.shapeB, Clamp01(c.shapeB), k);
        const float inh      = Smooth(sh_.inharm, Clamp01(c.inharm), k);
        const float spread   = Smooth(sh_.spread, Clamp01(c.spread), k);
        const float bShift   = Smooth(sh_.bandShift, Clamp01(c.bandShift), k);

        // Clamp the effective fundamental to a playable range. Without this, a
        // railed CV input (a gate or +5V source patched into V/OCT reads as +5
        // octaves) pushes every partial past Nyquist: the anti-alias fade mutes
        // the whole bank and the module goes silent with all bands "starved" —
        // which looks exactly like a crash. Railed input should mean "very high
        // note", not "dead module".
        float f0 = c.pitchHz * std::exp2(c.pitchCv + c.fineTune * (1.f / 12.f));
        f0       = std::fminf(std::fmaxf(f0, kF0Min), kF0Max);
        const float hzToInc = 4294967296.f / sampleRate_; // 2^32 / sr
        const float nyqFade = 1.f / (kNyqFadeFrac * nyquist_);

        // Window, in partial-index space, exponential so the low partials (where
        // the ear cares) get most of the knob travel.
        const float nf       = (float)kNumPartials;
        const float logN     = FastLog2(nf);
        const float winStart = std::exp2(position * logN);
        const float winWidth = std::exp2(window * logN);
        const float winEnd   = winStart + winWidth;
        winStart_ = winStart; // published for the emulator's partial viewer
        winEnd_   = winEnd;

        // Smoothed here rather than after the band loop because phi needs it.
        paritySmooth_ += (Clamp01(c.parity) - paritySmooth_) * smoothCoefBlk_;

        // Mode params.
        const bool  shepard   = (c.mode == kModeShepard);
        // Phi's range doubles under ODD ONLY. The wrap is seamless only when the
        // shift lands the surviving set back on itself: at phi=1 the odd set
        // {1,3..95} sits on {2,4..96}, the EVEN positions, so it jumps. At phi=2
        // it sits on {3,5..97} — itself, relabelled. Per block, so it is free.
        const float phi       = shapeA * (1.f + paritySmooth_);
        // Shepard's knob 5 ducks the windowed partials, INVERTED so the knob at
        // rest (fully CCW) means unity gain. As a plain gain, the knob sitting
        // at zero silenced everything inside the window — with knob 3 wide that
        // is the whole bank, so Shepard sounded "dead" at the exact physical
        // knob positions where Cluster (knob 5 = density) sounded fine.
        const float winGain   = shepard ? 1.f - shapeB : 1.f;
        const float density   = std::fminf(shapeB, kDensityMax);
        // Partials per cluster: soft-step, integer part sets the grouping and the
        // fraction crossfades between groupings. Ceiling is half an octave past
        // kNumPartials so the knob can reach a single cluster. logN, not log2():
        // the latter promotes to double and calls libm once per block.
        const float mF   = std::exp2(shapeA * (logN + 0.5f));
        const float mLo  = std::floor(mF);
        const float mFrac = mF - mLo;
        const float mHi  = mLo + 1.f;
        const float invLo = 1.f / mLo;
        const float invHi = 1.f / mHi;

        // Cluster collapse concentrates the bank: a cluster's partials span
        // m - 1 ratio steps (m of them, one step apart), compressed by (1-d),
        // and the 1/sqrt(r) tilt makes the pile louder as it slides down.
        // Summed power under that tilt is ~ln(1+span/s)/span, so with
        // x = (m-1)/s and y = x(1-d) the compressed:spread power ratio is
        // R(y)/R(x), R(t) = ln(1+t)/t. m-1, not m: at m = 1 nothing shifts,
        // and the span form makes the boost exactly 1 there for any density.
        // Undoing the boost holds RMS flat across the density knob, which is
        // what keeps Cluster off the clip.
        //
        // Derived from control values ONLY. A gain computed from the engine's
        // own amp state instead produced hardware-only ~8 kHz artifacts twice
        // (archives.md); that feedback structure must not come back.
        const float s0    = std::fmaxf(winStart, 1.f);
        const float nWin  = std::fmaxf(std::fminf(winEnd, nf) - s0, 1.f);
        const float x     = std::fminf(mF - 1.f, nWin) / s0;
        const float y     = x * (1.f - density);

        tiltSmooth_ += (Clamp01(c.tilt) - tiltSmooth_) * smoothCoefBlk_;
        tiltInv_ = 1.f - tiltSmooth_;

        // R(t) above is the ln form ONLY for the 1/sqrt(r) tilt. Integrating
        // r^-2a over the collapsed cluster gives ln(1+t)/t at a = 1/2 but
        // 1/(1+t) at a = 1, so the sawtooth slope needs its own ratio,
        // (1+x)/(1+y). The tilt morphs between the two, so the boosts do too:
        // exact at both ends, and the middle is not a power law anyway.
        // Missing this is what made the density knob swing 6.4 dB when the tilt
        // changed — the compensation was still solving the old spectrum.
        const float boostDark   = (1.f + x) / (1.f + y);
        const float boostBright = Log1pOverX(y) / Log1pOverX(x);
        const float boost = boostDark + tiltSmooth_ * (boostBright - boostDark);
        // Smoothed at block rate like the band gains: knob 5 moves this ~2.6x
        // faster than Shepard's duck moves winGain, and a gain stepping at
        // block rate is an AM sideband at the callback rate (whinebug.md).
        const float norm = (shepard || !FOXTAIL_CLUSTER_NORM) ? 1.f : FastRSqrt(boost);
        // Level compensation for the tilt morph, riding into the band gains
        // beside `norm` for the same reason: both are per-block values the
        // partial loop already multiplies in. Summed over the bank, equal power
        // per octave is ~5 dB louder than a sawtooth slope, so GATE would step
        // the level instead of only the colour. Quadratic fit to
        // sqrt(P(0)/P(m)), worst error 0.06 dB. Like the Cluster compensation it
        // assumes a flat envelope: with only a few low bands up the two slopes
        // are much closer together and this over-corrects a little.
        const float tiltNorm = 1.f + tiltSmooth_ * (kTiltNorm1 + tiltSmooth_ * kTiltNorm2);

        // One smoothed gain per band, not an L/R pair: pan left the bands when
        // the spread became global, which is what pays for the slot lookup.
        //
        // The Cluster compensation rides in here, not in the partial loop:
        // these are already per-block values that the loop multiplies in, so
        // the hot path keeps the exact instruction sequence it was tuned with,
        // and the band smoother doubles as the compensation's smoother. Two
        // extra instructions in the partial loop brought the Cluster overtones
        // back with the audio bit-identical (archives.md) — keep it out of there.
        for (int b = 0; b < kNumBands; ++b)
        {
            // kHeadroom rides in here rather than the partial loop: it is a
            // constant, and everything it scaled flows through this gain anyway.
            const float tg = Clamp01(c.bandGain[b]) * norm * tiltNorm * kHeadroom;
            bandGainSmooth_[b] += (tg - bandGainSmooth_[b]) * smoothCoefBlk_;
            // Flush the one-pole tails to zero. Left alone they decay into
            // denormals, and denormal arithmetic on the M7 is slow enough to
            // show up as a transient CPU spike once the whole bank is doing it.
            if (bandGainSmooth_[b] < 1e-20f && bandGainSmooth_[b] > -1e-20f)
                bandGainSmooth_[b] = 0.f;
            meter_[b]   = Clamp01(c.bandGain[b]);
            audible_[b] = bandLoRatio_[b] * f0 < nyquist_;

            // Comb geometry, folded so the partial loop needs one subtract and
            // one clamp. seed = where the band fills from; gate = how far the
            // fill has reached, in partials, +1 so a partial at the frontier
            // fades over its last step instead of switching off.
            const float sc  = Clamp01(c.bandShape[b]); // measured from the TOP
            const float far = (sc > 0.5f ? sc : 1.f - sc) * bandSpanW_[b];
            const float fil = Clamp01(Clamp01(c.bandGain[b]) * 2.f - 1.f);
            bandSeed_[b] += (bandSpanHi_[b] - sc * bandSpanW_[b] - bandSeed_[b])
                            * smoothCoefBlk_;
            bandGate_[b] += (fil * far + 1.f - bandGate_[b]) * smoothCoefBlk_;
        }
        // Step up to the next band's gain, for the Shepard crossfade. A second
        // pass because band b needs band b+1 already smoothed.
        for (int b = 0; b < kNumBands; ++b)
        {
            const int b1 = b + 1 > kNumBands - 1 ? kNumBands - 1 : b + 1;
            bandGainStep_[b] = bandGainSmooth_[b1] - bandGainSmooth_[b];
        }

        // Slot pan table, stored as offsets FROM centre so the partial loop can
        // lerp centre -> slot with two FMAs. sqrt is affordable here: 8 turns
        // per block, not 96. Lerping between two equal-power points is not
        // itself equal power, but the dip is a fraction of a dB.
        //
        // Pot 3 picks a segment between two anchors and a position within it.
        // Morphing in POSITION space, not gain space, is what keeps this at
        // 2 * kSlots square roots however many anchors the table grows to.
        int seg = 0;
        while (seg < kPanAnchors - 2 && spread > kPanKnot[seg + 1]) ++seg;
        const float segT = Clamp01((spread - kPanKnot[seg])
                                   / (kPanKnot[seg + 1] - kPanKnot[seg]));

        // segT saturates at the ORBIT knot, so above it the shape is fixed and
        // the remaining travel buys motion: rate first, then the two parity
        // subsets pulling apart. Rotation only exists in the top segment, and
        // its rate ramps from zero at SCATTER, so no knot starts motion abruptly.
        const bool  rotating = seg + 1 == kPanRotAnchor;
        const float motion   = Clamp01((spread - kPanKnot[kPanRotAnchor])
                                       / (1.f - kPanKnot[kPanRotAnchor]));
        const float rate     = rotating ? segT * segT
                                              * (kPanRotSlow
                                                 + motion * (kPanRotFast - kPanRotSlow))
                                        : 0.f;

        // Advancing by the ramped rate rather than gating a free-running phase
        // means a parked knob holds the orbit where it is.
        panRotPhA_ += rate * blockSec_;
        panRotPhB_ += rate * (1.f + motion * (kPanRotCounter - 1.f)) * blockSec_;
        panRotPhA_ -= std::floor(panRotPhA_);
        panRotPhB_ -= std::floor(panRotPhB_); // rate can be negative; floor still wraps

        for (int i = 0; i < kSlots; ++i)
        {
            const float ph    = kPanRotSub[i] ? panRotPhB_ : panRotPhA_;
            const float orbit = kPanRotAmp * SineTurns(ph + kPanRotOfs[i]);
            const float a     = kPanAnchor[seg][i];
            const float b     = rotating ? orbit : kPanAnchor[seg + 1][i];
            const float p     = 0.5f + 0.5f * (a + (b - a) * segT);
            panDelta_[2 * i]     = std::sqrt(1.f - p) - kCentre;
            panDelta_[2 * i + 1] = std::sqrt(p) - kCentre;
        }

        // Slides the envelope and the comb together along the harmonic series.
        // One multiply on sel in the partial loop covers both, because sel is
        // the only thing either of them is indexed by.
        bandShift_ = std::exp2((0.5f - bShift) * (2.f * kBandShiftOct));
        bpShift_   = FastLog2(bandShift_) * bandScale_ - 0.5f;


        // Inharmonicity coefficient. Exponential so the musically useful low end
        // (piano-ish stretch) gets most of the slider travel.
        const float inharmB = inh <= 0.f
                                  ? 0.f
                                  : kInharmMax * (std::exp2(inh * 8.f) - 1.f) / 255.f;

        // Window edges are tapered, not hard. A boolean edge means a partial
        // crossing it jumps frequency instantly, so sweeping SPREAD or FEEDBACK
        // drags partials across one after another and you hear a burst of clicks
        // — the "scratchy knob". The taper blends each partial's shift in and out
        // instead, at the cost of one multiply. It sits OUTSIDE the window: a
        // partial at winStart is already fully shifted. Inside, a full-width
        // window left the top ~9 partials stranded near Nyquist.
        const float edge    = winWidth * 0.1f > 1.f ? winWidth * 0.1f : 1.f;
        const float invEdge = 1.f / edge;
        const float loEdge  = winStart - edge;
        const float hiEdge  = winEnd + edge;

        for (int k = 0; k < kNumPartials; ++k)
        {
            const float idx = (float)(k + 1); // partial 1 is the fundamental

            // Trapezoid with smoothstep shoulders: 0 outside, 1 well inside.
            const float wLin = Clamp01(std::fminf((idx - loEdge) * invEdge,
                                                  (hiEdge - idx) * invEdge));
            const float w    = wLin * wLin * (3.f - 2.f * wLin);

            float shifted = idx;
            if (shepard)
            {
                shifted = idx + phi;
            }
            else
            {
                const float rel = std::fmaxf(idx - winStart, 0.f);
                const float cLo = ClusterStart(rel, mLo, invLo);
                const float cHi = ClusterStart(rel, mHi, invHi);
                const float s   = winStart + cLo + (cHi - cLo) * mFrac;
                shifted         = idx + density * (s - idx);
            }

            float ratio = idx + w * (shifted - idx);

            // Stiff-string inharmonicity: f_k = k*f0*sqrt(1 + B*k^2).
            // Branchless on purpose (an if() here makes GCC clone the loop
            // body). std::sqrt is safe ONLY because the build sets
            // -fno-math-errno: that makes it a bare VSQRT, exact at B = 0.
            ratio *= std::sqrt(1.f + inharmB * ratio * ratio);
            // Shepard's ends. The bank is finite, so a shift by one index leaves
            // nothing at the bottom and adds one at the top: the fundamental
            // simply vanishes as phi rises. Fading the extreme ends to silence
            // makes the wrap exact instead of nearly exact — at phi=0 the set is
            // {2..96} plus a silent 1, at full phi it is {2..96} plus a silent
            // 97, which are the same sound. One partial wide, so only the
            // outermost two are ever touched, and each fades across a whole phi
            // sweep. Scaled by w so partials outside the window, which never
            // moved, are not faded for standing still.
            const float ends  = Clamp01(std::fminf(ratio - 1.f, kEndFade - ratio));
            const float wGain = shepard ? 1.f + w * (winGain * ends - 1.f) : 1.f;

            const float freq = ratio * f0;

            // Clamp to Nyquist before the integer conversion. Partial 512 at
            // f0 = 440 Hz wants 225 kHz, whose increment overflows the
            // float->integer conversion outright (UB). Those partials are muted
            // by the fade below, so pinning their increment costs nothing.
            const float incHz = std::fminf(freq, nyquist_);
            inc_[k]           = (uint32_t)(incHz * hzToInc);

            // Shepard samples the envelope at the SHIFTED frequency: at phi = 1
            // partial k sits where partial k+1 was, so if amplitude depends only
            // on frequency the ensemble before and after the wrap is identical
            // and nothing clicks. Cluster samples it at the partial INDEX
            // instead, so a slider always owns the same harmonics no matter
            // where the collapse dragged them — otherwise sweeping density
            // empties the top of the spectrum and the upper sliders go dead
            // under your fingers. Sliders pick the ingredients in Cluster and
            // EQ the output in Shepard; the branch is unswitched, so it is free.
            const float sel = (shepard ? ratio : idx) * bandShift_;

            // Slider b owns the partials from its own position up to slider b+1
            // (hence the -0.5). Cluster gets no crossfade into the neighbour:
            // the comb picks partials strictly per band, so blending the gain
            // across the boundary meant a band's survivors drew their level from
            // the slider next door — set a band to highpass and it kept sounding
            // with its own slider down.
            //
            // Shepard needs one anyway: partials slide through the envelope, and
            // a step at a boundary is an audible tick as each one crosses. It is
            // kXfadeW of a band wide instead of the old 35%, linear instead of
            // smoothstepped, and folded to one load plus one FMA. Free in the
            // budget that matters — `shepard` is loop-invariant, so GCC
            // unswitches it and the Cluster clone carries none of this.
            // Cluster indexes the envelope by partial number, so log2(sel) is
            // log2(k+1) + log2(bandShift) and the first term depends on nothing
            // a control can move — it is a table, not a computation. Shepard
            // indexes by the shifted ratio, which really does vary, so it keeps
            // the call. This was the single most expensive line in the loop.
            const float bp = shepard ? FastLog2(sel) * bandScale_ - 0.5f
                                     : bpIdx_[k] + bpShift_;
            int         b  = (int)bp;
            if (bp < 0.f) b = 0;
            if (b > kNumBands - 1) b = kNumBands - 1;

            const float gb = bandGainSmooth_[b];
            const float g  = shepard
                                 ? gb + bandGainStep_[b]
                                            * Clamp01((bp - (float)b - kXfadeLo) * kXfadeInv)
                                 : gb;

            // Comb: the band fills outward from its seed, one partial at a time.
            // Distance is in partials, so a pot means the same thing in a band
            // holding 2 of them and one holding 32.
            const float fg = Clamp01(bandGate_[b] - std::fabs(sel - bandSeed_[b]));

            // Pan by partial number, not by band. The fundamental stays centred
            // and everything above it sits at its slot — an image anchored at
            // the bottom, which a fully spread fundamental smears.
            const int   sl = kSlotPat[kSlots - 1][k & (kSlots - 1)];
            const float sr = Clamp01(ratio - 1.f);
            const float gc = g * kCentre;
            const float gs = g * sr;
            const float gl = gc + gs * panDelta_[2 * sl];
            const float gr = gc + gs * panDelta_[2 * sl + 1];

            // Spectral tilt, morphing between the two slopes worth having.
            // rt*rt = 1/ratio, a sawtooth's -6 dB/octave: harmonics are evenly
            // spaced in frequency, so the octave above r holds r partials and
            // each successive one carries 1/r of the power — falling, the way an
            // acoustic source does. rt = 1/sqrt(ratio) gives every octave EQUAL
            // power instead, which makes every band slider equally effective but
            // sounds 3 dB/octave brighter than a sawtooth, ~20 dB over the bank.
            //
            // A lerp, not a branch: `shepard` already splits this loop in two and
            // a second bool would make it four. Three instructions, no clone, and
            // the in-between positions are usable slopes rather than a crossfade
            // between two spectra.
            const float rt   = FastRSqrt(ratio);
            const float tilt = rt * (rt * tiltInv_ + tiltSmooth_);

            // Anti-alias fade, branch-free.
            const float fade = Clamp01((nyquist_ - freq) * nyqFade);

            // Parity: fade the even-numbered partials out. Partial number is
            // k+1, so k odd == an even partial.
            const float par = (k & 1) ? (1.f - paritySmooth_) : 1.f;

            const float a = tilt * fade * wGain * par * fg;
            // No denormal guard here: the band-gain smoothers are flushed, so a
            // denormal cannot originate upstream, and two compares per partial
            // per block is not free.
            ampL_[k] = gl * a;
            ampR_[k] = gr * a;
        }
    }

    float sampleRate_;
    float nyquist_;
    float  smoothCoef_;    // per sample  (master)
    float  smoothCoefBlk_; // per block   (band gains/pans, parity, shaper set)
    size_t lastFrames_;
    float  blockSec_;
    float  masterSmooth_;
    float  paritySmooth_;
    float  panRotPhA_; // ORBIT, even-k slot subset, in turns
    float  panRotPhB_; // ORBIT, odd-k slot subset (counter-rotates at full CW)

    // The controls that reach partial FREQUENCIES. Grouped because they share a
    // reason to be smoothed and a coefficient; the gains are smoothed elsewhere
    // (folded, per band) because they reach amplitude instead.
    struct Shaper
    {
        float position, window, shapeA, shapeB, inharm, spread, bandShift;
    };
    Shaper sh_;

    float winStart_;
    float winEnd_;
    float bandScale_;  // log2(ratio) -> band position

    float bandGainSmooth_[kNumBands];
    float bandGainStep_[kNumBands]; // gain[b+1] - gain[b], for the Shepard crossfade
    float bandSeed_[kNumBands];     // partial the band fills out from
    float bandGate_[kNumBands];     // how far the fill has reached, in partials
    float bandSpanLo_[kNumBands];
    float bandSpanHi_[kNumBands];
    float bandSpanW_[kNumBands];
    float tiltSmooth_;              // GATE is a step; this is what keeps it quiet
    float tiltInv_;                 // 1 - tiltSmooth_, hoisted out of the loop
    float panDelta_[2 * kSlots];    // offsets from centre, per slot, L/R interleaved
    float bandShift_;               // multiplier on sel: slides envelope + comb
    float bpShift_;                 // log2(bandShift_) folded with the -0.5
    float bpIdx_[kNumPartials];     // FastLog2(k+1) * bandScale_, Cluster's band lookup
    float meter_[kNumBands];
    bool  audible_[kNumBands];
    float bandLoRatio_[kNumBands];

    uint32_t phase_[kNumPartials];
    uint32_t inc_[kNumPartials];
    float    ampL_[kNumPartials];
    float    ampR_[kNumPartials];

    float sine_[kSineSize];
};

} // namespace foxtail