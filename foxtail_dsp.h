#pragma once
//
// Fox Tail — additive sine oscillator DSP.
//
// This header is the "seam": pure DSP, no libDaisy, no hardware types. It is
// included by BOTH the firmware (FoxTail.cpp fills Controls from the Daisy
// hardware) and the desktop emulator (emulator/ fills Controls from the GUI),
// so the exact same synthesis runs in both places.
//
// Units are normalized, matching what time_machine_hardware.cpp already hands
// the DSP: knob/slider values are 0..1, CV values are -1..+1. No volts here.
//
#include <cmath>
#include <cstddef>

namespace foxtail {

static constexpr int   kNumHarmonics = 8;
static constexpr float kTwoPi        = 6.283185307179586f;

// Normalized control snapshot, filled once per audio block by the host
// (firmware or emulator). Grows as the instrument grows; milestone 1 uses only
// amp[0], pitchHz and master.
struct Controls {
    float amp[kNumHarmonics] = {0.f}; // per-harmonic amplitude, 0..1
    float pitchHz            = 220.f; // fundamental frequency in Hz
    float master             = 0.7f;  // master output level, 0..1
};

// The oscillator. Plain class, no virtuals — zero dispatch cost in the audio
// loop. One phase accumulator per harmonic (only harmonic 0 active in v1).
class FoxTailOsc {
  public:
    void Init(float sampleRate) {
        sampleRate_ = sampleRate;
        // One-pole smoothing coefficient for gains (~5 ms time constant). Gains
        // are ramped per sample toward their target so a moved control can't
        // step the waveform at a block boundary (that step is the "crackle").
        smoothCoef_ = 1.f - std::exp(-1.f / (kGainSmoothSec * sampleRate_));
        masterSmooth_ = 0.f;
        for (int h = 0; h < kNumHarmonics; ++h) {
            phase_[h] = 0.f;
            ampSmooth_[h] = 0.f;
            meter_[h] = 0.f;
        }
    }

    // Render one block into separate L/R buffers (matches the Daisy callback's
    // out[0][i]/out[1][i] layout). Constant work per call — no data-dependent
    // branching in the hot loop (noise-budget rule from whinebug.md).
    void Process(const Controls& c, float* outL, float* outR, size_t frames) {
        // Precompute per-harmonic phase increments (frequency changes are
        // click-free, so no smoothing needed on these).
        float inc[kNumHarmonics];
        for (int h = 0; h < kNumHarmonics; ++h)
            inc[h] = (c.pitchHz * (h + 1)) / sampleRate_; // harmonic h -> (h+1)*f0

        for (size_t i = 0; i < frames; ++i) {
            // Ramp the master gain toward its target, once per sample.
            masterSmooth_ += (c.master - masterSmooth_) * smoothCoef_;

            float s = 0.f;
            for (int h = 0; h < kNumHarmonics; ++h) {
                // Ramp each harmonic's amplitude too (this is what kills the
                // slider crackle).
                ampSmooth_[h] += (c.amp[h] - ampSmooth_[h]) * smoothCoef_;
                s += std::sin(phase_[h] * kTwoPi) * ampSmooth_[h];
                phase_[h] += inc[h];
                if (phase_[h] >= 1.f) phase_[h] -= 1.f;
            }
            s *= masterSmooth_;
            outL[i] = s;
            outR[i] = s;
        }

        // Level meters for the LEDs follow the smoothed amplitude.
        for (int h = 0; h < kNumHarmonics; ++h) meter_[h] = ampSmooth_[h];
    }

    // Per-harmonic level for the 9 panel LEDs (0..1).
    float Meter(int harmonic) const { return meter_[harmonic]; }

  private:
    static constexpr float kGainSmoothSec = 0.005f; // gain ramp time constant

    float sampleRate_ = 48000.f;
    float smoothCoef_ = 1.f;
    float phase_[kNumHarmonics];
    float ampSmooth_[kNumHarmonics]; // per-harmonic smoothed gain
    float masterSmooth_ = 0.f;
    float meter_[kNumHarmonics];
};

} // namespace foxtail
