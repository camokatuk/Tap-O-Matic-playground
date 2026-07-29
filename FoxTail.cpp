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
//   * Constant work per callback (every partial is always rendered, silent ones
//     included; no data-dependent loop, no sub-rate update pattern).
//   * Small block size (5 @ 48 kHz -> callback spur at 9.6 kHz, least audible).
//   * Audio input is never read or passed through.
//
#include "daisy_patch_sm.h"
#include "time_machine_hardware.h"
#include "foxtail_dsp.h"

#include <cmath>

using namespace daisy;
using namespace oam::time_machine;

// ---------------------------------------------------------------------------
// Panel vocabulary.
//
// Controls are numbered left to right as they sit on the module. The board
// support is shared with the delay, so its accessors keep their delay-era names
// (GetSpreadKnob, GetFeedbackKnob, ...) — this block is the single place those
// old names appear, and everything below refers to numbers.
//
//   Knob 1  (was TIME)      -> fundamental pitch; its jack is V/OCT
//   Knob 2  (was SPREAD)    -> shaper window start
//   Knob 3  (was FEEDBACK)  -> shaper window width
//   Knob 4  (was HIGHPASS)  -> cluster: partials per cluster | shepard: phi
//   Knob 5  (was LOWPASS)   -> cluster: density              | shepard: win gain
//   Switch 1 (left)         -> shaper mode: CLUSTER / SHEPARD
//   Switch 2 (right)        -> parity: all partials / odd only
//   Slider b, Pot b         -> band b's gain and pan
// ---------------------------------------------------------------------------

TimeMachineHardware hw;

static inline float Knob1() { return hw.GetTimeKnob(); }
static inline float Knob2() { return hw.GetSpreadKnob(); }
static inline float Knob3() { return hw.GetFeedbackKnob(); }
static inline float Knob4() { return hw.GetHighpassKnob(); }
static inline float Knob5() { return hw.GetLowpassKnob(); }

static constexpr int kKnob1Cv = TIME_CV;
static constexpr int kKnob2Cv = SPREAD_CV;
static constexpr int kKnob3Cv = FEEDBACK_CV;
static constexpr int kKnob4Cv = HIGHPASS_CV;
static constexpr int kKnob5Cv = LOWPASS_CV;

// --- Bring-up diagnostics ---------------------------------------------------
//   0 = normal: LEDs show per-band level
//   1 = LEDs are a 9-segment CPU load bar (all nine lit = 100% = overrunning)
//   2 = LED 1 shows switch 1, LED 9 shows switch 2 — for working out which
//       physical position reads HIGH, so the panel can be labelled correctly
// Modes 1 and 2 also log over USB serial once a second. Set to 0 to ship.
#define FOXTAIL_LED_MODE 2

static constexpr float kFreqMin = 20.0f;
static constexpr float kFreqMax = 2000.0f;
static constexpr float kMaster  = 0.7f;
static constexpr float kPitchCvOctaves = 5.0f; // V/OCT jack full-scale -> +-octaves

Led  leds[foxtail::kNumBands]; // one LED per band (9)
GPIO switch1;                  // left  — CLUSTER / SHEPARD
GPIO switch2;                  // right — parity: all / odd only
#if FOXTAIL_LED_MODE != 0
CpuLoadMeter loadMeter;
#endif

// The oscillator carries the partial state plus the sine table. Park it in DTCM:
// uncached, zero wait state, no bus arbitration, so every sample costs the same.
// The default .bss lands in AXI SRAM, which is cached — refills burst, and bursty
// is exactly what the whine feeds on. (See control-maps.md, "Memory".)
// Diagnostic mode 1/2 prints this object's address: 0x2000xxxx means DTCM took,
// 0x24xxxxxx means it did not and the inner loop is paying cache-miss latency.
foxtail::FoxTailOsc osc DTCM_MEM_SECTION;
foxtail::Controls   controls;

static inline float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

// Panel knob (inverted, as the delay firmware does) summed with its CV jack.
static inline float knobPlusCv(float knob, int cvAdc)
{
    return clampf((1.0f - knob) + hw.GetAdcValue(cvAdc), 0.0f, 1.0f);
}

// Audio callback: fill the seam from hardware, then render. Input `in` is
// intentionally ignored (noise budget). Non-interleaved out[ch][frame].
void audioCallback(AudioHandle::InputBuffer  /*in*/,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
#if FOXTAIL_LED_MODE != 0
    loadMeter.OnBlockStart();
#endif

    hw.ProcessAllControls();

    // Sliders and pots are both inverted (up / clockwise = more) — measured on
    // the board. Level CVs are bipolar and sum into the band gain.
    for (int b = 0; b < foxtail::kNumBands; ++b)
    {
        float base = 1.0f - hw.GetLevelSlider(b);
        controls.bandGain[b] = clampf(base + hw.GetLevelCV(b), 0.0f, 1.0f);
        controls.bandPan[b]  = clampf(1.0f - hw.GetPanKnob(b), 0.0f, 1.0f);
    }

    // Knob 1: pitch, log sweep, with the V/OCT jack added in octaves. That jack
    // is uncalibrated, so an unpatched input may read slightly off zero.
    float t          = clampf(1.0f - Knob1(), 0.0f, 1.0f);
    controls.pitchHz = kFreqMin * powf(kFreqMax / kFreqMin, t);
    controls.pitchCv = hw.GetAdcValue(kKnob1Cv) * kPitchCvOctaves;
    controls.master  = kMaster;

    controls.position = knobPlusCv(Knob2(), kKnob2Cv);
    controls.window   = knobPlusCv(Knob3(), kKnob3Cv);
    controls.shapeA   = knobPlusCv(Knob4(), kKnob4Cv);
    controls.shapeB   = knobPlusCv(Knob5(), kKnob5Cv);

    // Both switches are pulled up, so Read() is HIGH when the contact is open.
    // Which physical position that is, is exactly what LED mode 2 is for.
    controls.mode   = switch1.Read() ? foxtail::kModeCluster : foxtail::kModeShepard;
    controls.parity = switch2.Read() ? 0.0f : 1.0f;

    osc.Process(controls, out[0], out[1], size);

#if FOXTAIL_LED_MODE != 0
    loadMeter.OnBlockEnd();
#endif
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

    switch1.Init(FEEDBACK_MODE_SWITCH, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    switch2.Init(FILTER_POSITION_SWITCH, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);

    osc.Init(hw.AudioSampleRate());

#if FOXTAIL_LED_MODE != 0
    loadMeter.Init(hw.AudioSampleRate(), 5);
    hw.StartLog(false); // non-blocking: runs with or without a serial monitor
#endif

    hw.StartAudio(audioCallback);

    // Drive LEDs from the main loop (not the audio IRQ) to avoid FPU
    // lazy-stacking corruption of Led::Update()'s pwm accumulator.
    uint32_t last_led_ms = 0;
    uint32_t last_log_ms = 0;
    (void)last_log_ms;

    while (true)
    {
        uint32_t now = System::GetNow();
        if (now - last_led_ms >= 1)
        {
#if FOXTAIL_LED_MODE == 1
            // 9-segment CPU load bar: segment i fills between i/9 and (i+1)/9.
            const float load = loadMeter.GetMaxCpuLoad();
            for (int i = 0; i < foxtail::kNumBands; i++)
                leds[i].Set(clampf(load * foxtail::kNumBands - (float)i, 0.f, 1.f));
#elif FOXTAIL_LED_MODE == 2
            // Switch states only: LED 1 = switch 1 (left), LED 9 = switch 2
            // (right), lit when that switch reads HIGH.
            for (int i = 0; i < foxtail::kNumBands; i++) leds[i].Set(0.f);
            leds[0].Set(switch1.Read() ? 1.f : 0.f);
            leds[foxtail::kNumBands - 1].Set(switch2.Read() ? 1.f : 0.f);
#else
            for (int i = 0; i < foxtail::kNumBands; i++) leds[i].Set(osc.Meter(i));
#endif
            for (int i = 0; i < foxtail::kNumBands; i++) leds[i].Update();
            last_led_ms = now;
        }

#if FOXTAIL_LED_MODE != 0
        if (now - last_log_ms >= 1000)
        {
            hw.PrintLine("partials=%d  osc@%p (0x2000.. = DTCM ok)  "
                         "sw1=%d sw2=%d  cpu avg=" FLT_FMT(1) "%% max=" FLT_FMT(1) "%%",
                         foxtail::kNumPartials,
                         (void*)&osc,
                         switch1.Read() ? 1 : 0,
                         switch2.Read() ? 1 : 0,
                         FLT_VAR(1, loadMeter.GetAvgCpuLoad() * 100.f),
                         FLT_VAR(1, loadMeter.GetMaxCpuLoad() * 100.f));
            last_log_ms = now;
        }
#endif
    }
}
