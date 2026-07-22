//
// Fox Tail — additive sine oscillator firmware for the Daisy Patch SM.
//
// This file is the "hard stuff": hardware bootstrap + audio plumbing. It is
// deliberately thin. ALL synthesis lives in the shared, hardware-free
// foxtail_dsp.h (the same file the emulator runs), so what you hear on the
// bench is what you hear on the module.
//
// Noise budget (see docs/claude/whinebug.md) — this firmware obeys all of it:
//   * No external-RAM (SDRAM) usage.
//   * Constant work per callback (fixed partial count, no data-dependent loop).
//   * Small block size (5 @ 48 kHz -> callback spur at 9.6 kHz, least audible).
//   * Audio input is never read or passed through.
//
#include "daisy_patch_sm.h"
#include "time_machine_hardware.h"
#include "foxtail_dsp.h"

#include <cmath>

using namespace daisy;
using namespace oam::time_machine;

// --- Milestone 1 control mapping -------------------------------------------
// DRY slider  -> amplitude of one sine (harmonic 1).
// TIME knob   -> its frequency, 20..2000 Hz on a log law.
// V/OCT jack  -> pitch modulation (patch an external envelope/CV here to test).
// Everything else on the panel is unused for now.
static constexpr float kFreqMin = 20.0f;
static constexpr float kFreqMax = 2000.0f;
static constexpr float kMaster  = 0.7f;
static constexpr float kPitchCvOctaves = 5.0f; // V/OCT jack full-scale -> ±octaves

TimeMachineHardware hw;
Led                 leds[foxtail::kNumHarmonics]; // one LED per harmonic (9)
foxtail::FoxTailOsc osc;
foxtail::Controls   controls;

static inline float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

// Audio callback: fill the seam from hardware, then render. Input `in` is
// intentionally ignored (noise budget). Non-interleaved out[ch][frame].
void audioCallback(AudioHandle::InputBuffer  /*in*/,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    hw.ProcessAllControls();

    // Each harmonic: effective amplitude = clamp(slider + its level CV, 0..1).
    // Slider values arrive normalized (0..1); the '1.0 - x' matches the panel
    // orientation (up = more). Level CVs are bipolar (-1..1).
    for (int h = 0; h < foxtail::kNumHarmonics; ++h)
    {
        float base = 1.0f - hw.GetLevelSlider(h);
        controls.amp[h] = clampf(base + hw.GetLevelCV(h), 0.0f, 1.0f);
    }

    float t          = clampf(1.0f - hw.GetTimeKnob(), 0.0f, 1.0f);
    controls.pitchHz = kFreqMin * powf(kFreqMax / kFreqMin, t); // log sweep
    controls.master  = kMaster;

    // V/OCT jack (the analog TIME_CV input) -> pitch modulation in octaves.
    // Uncalibrated: an unpatched jack may read slightly off-zero.
    controls.pitchCv = hw.GetAdcValue(TIME_CV) * kPitchCvOctaves;

    osc.Process(controls, out[0], out[1], size);
}

int main(void)
{
    hw.Init();

    hw.SetAudioBlockSize(5); // keep the callback spur high (see noise budget)
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    leds[0].Init(LED_DRY, false);
    leds[1].Init(LED_1, false);
    leds[2].Init(LED_2, false);
    leds[3].Init(LED_3, false);
    leds[4].Init(LED_4, false);
    leds[5].Init(LED_5, false);
    leds[6].Init(LED_6, false);
    leds[7].Init(LED_7, false);
    leds[8].Init(LED_8, false);

    osc.Init(hw.AudioSampleRate());

    hw.StartAudio(audioCallback);

    // Drive LEDs from the main loop (not the audio IRQ) to avoid FPU
    // lazy-stacking corruption of Led::Update()'s pwm accumulator.
    uint32_t last_led_ms = 0;
    while (true)
    {
        uint32_t now = System::GetNow();
        if (now - last_led_ms >= 1)
        {
            // Each LED shows its harmonic's level.
            for (int i = 0; i < foxtail::kNumHarmonics; i++)
            {
                leds[i].Set(osc.Meter(i));
                leds[i].Update();
            }
            last_led_ms = now;
        }
    }
}
