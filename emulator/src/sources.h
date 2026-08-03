#pragma once
//
// Fox Tail emulator — modulation sources (envelopes, oscillators, ...).
//
// These stand in for the external Eurorack modules you'd patch into the CV
// jacks on the real hardware. They exist ONLY in the emulator: the firmware
// reads real jack voltages instead, so this file is never compiled into it.
//
#include <algorithm>
#include <cmath>

namespace ftemu {

// Simple attack/decay envelope. Times in seconds; curve in [-1, +1]
// (-1 = logarithmic / fast-then-slow, 0 = linear, +1 = exponential).
// Output is 0..1. Advance once per audio block with process(blockSeconds).
struct Envelope {
    enum Stage { Idle, Attack, Decay };

    Stage stage  = Idle;
    float phase  = 0.f;      // 0..1 ramp position within the current stage
    // These are in SECONDS. The emulator overwrites them each block from the
    // UI, whose sliders are a normalized 0..1 position mapped by normToSec()
    // (0->1ms, 1->10s). So a UI value of 0.4 becomes ~0.04 s here, NOT 0.4 s.
    float attack  = 0.002f;  // seconds
    float decay   = 0.040f;  // seconds
    float curve   = 0.f;     // -1..+1
    bool  cycling = false;   // loop attack->decay->attack instead of stopping

    void trigger() { stage = Attack; phase = 0.f; }

    float process(float dt) {
        if (stage == Attack) {
            phase += dt / std::max(attack, 1e-4f);
            if (phase >= 1.f) { phase = 0.f; stage = Decay; }
        } else if (stage == Decay) {
            phase += dt / std::max(decay, 1e-4f);
            if (phase >= 1.f) {
                if (cycling) { phase = 0.f; stage = Attack; } // loop
                else         { phase = 1.f; stage = Idle; }
            }
        }

        // Linear 0..1 level for the current stage (attack rises, decay falls).
        float level = (stage == Attack) ? phase
                    : (stage == Decay)  ? (1.f - phase)
                                        : 0.f;

        // Curve shaping: gamma < 1 is concave (log-ish), > 1 convex (exp-ish).
        float gamma = std::exp2(curve * 2.f);
        return std::pow(std::max(0.f, level), gamma);
    }
};

// Slow shaped LFO, separate from Oscillator because it is a different job: that
// one spans the audio range off two summed Hz sliders, this one spans 0.001..20
// Hz off a single exponential slider and picks a waveform. Output is bipolar
// -1..1, starting at -1 so a cycle begins at the bottom of the target's range.
//
// Triangle and Saw are the two that matter for Shepard: a triangle glides phi up
// and back down with no discontinuity anywhere, a saw gives the endless version
// and leans on the wrap being seamless.
struct Lfo {
    enum Shape { Triangle, Sine, Saw, Square };

    float phase = 0.f;
    float freq  = 0.14f; // Hz
    int   shape = Triangle;

    float process(float dt) {
        phase += freq * dt;
        phase -= std::floor(phase);
        switch (shape) {
            case Sine:   return -std::cos(phase * 6.283185307179586f);
            case Saw:    return 2.f * phase - 1.f;
            case Square: return phase < 0.5f ? -1.f : 1.f;
            default:     return 1.f - 4.f * std::fabs(phase - 0.5f); // Triangle
        }
    }
};

// Sine LFO. freq in Hz. Advance once per audio block. Output is bipolar -1..1.
struct Oscillator {
    float phase = 0.f;
    float freq  = 1.f;

    float process(float dt) {
        phase += freq * dt;
        phase -= std::floor(phase);
        return std::sin(phase * 6.283185307179586f);
    }
};

} // namespace ftemu
