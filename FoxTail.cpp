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
// Bench facts that are easy to forget (all measured on this board):
//   * USB-only power leaves the Patch SM's analog domain dead: CV inputs rail,
//     audio is silent, pots keep working. Test CVs with rack power only.
//   * ADC readings shift slightly with load (whinebug coupling), so V/oct is
//     calibrated with audio running and LEDs dark — same conditions as playback.
//   * out[0] is the RIGHT jack: the output pair is crossed below libDaisy.
//
#include "daisy_patch_sm.h"
#include "time_machine_hardware.h"
#include "foxtail_dsp.h"
#include "foxtail_diag.h"

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
//   Knob 5  (was LOWPASS)   -> cluster: density | shepard: window duck (CCW = unity)
//   Switch 1 (left)         -> down = CLUSTER, up = SHEPARD
//   Switch 2 (right)        -> up = ALL partials, down = ODD ONLY
//                              (also the "capture" control during calibration)
//   Slider 1                -> inharmonicity (0 = pure harmonic series)
//   Pot 1                   -> fine tune, +-1 semitone (centre = in tune)
//   Sliders 2..9            -> gain of bands 1..8
//   Pots 2..9               -> pan of bands 1..8
//   LED 1                   -> inharmonicity amount
//   LEDs 2..9               -> per-band level, or the shaper window while you
//                              are turning knob 2 or 3 (see main loop)
// ---------------------------------------------------------------------------

TimeMachineHardware hw;

// --- Control normalization seam ---------------------------------------------
// Every panel read below goes through Slider() / PanPot() / BigKnob(), which
// return a PANEL-NORMALIZED value: 0 = fully CCW / fully down, 1 = fully CW /
// fully up, matching what the legend says. Keeping polarity in one place is
// what stops a stray `1.0f - x` from turning up at a call site. All are
// constant-argument at every call site, so they inline to nothing.
static constexpr int kKnobCv[5] = {TIME_CV, SPREAD_CV, FEEDBACK_CV, HIGHPASS_CV, LOWPASS_CV};
static constexpr int kKnob1Cv = TIME_CV; // V/OCT; named because calibration uses it

static inline float BigKnobRaw(int idx)
{
    switch (idx)
    {
        case 0: return hw.GetTimeKnob();
        case 1: return hw.GetSpreadKnob();
        case 2: return hw.GetFeedbackKnob();
        case 3: return hw.GetHighpassKnob();
        default: return hw.GetLowpassKnob();
    }
}

static inline float BigKnob(int idx) { return 1.0f - BigKnobRaw(idx); }
static inline float Slider(int idx) { return 1.0f - hw.GetLevelSlider(idx); }

static inline float PanPot(int idx) { return hw.GetPanKnob(idx); }

// Physical slider 1 is inharmonicity, pot 1 is fine tune; the bands start at 2.
static constexpr int kBandOffset = 1;
// First band with a shape pot: pots 2-3 are the spread pair, so the six left
// over land on bands 2..7.
static constexpr int kFirstShapeBand = 2;
static constexpr int kNumLeds    = 9; // one more than kNumBands (LED 1 = inharm)

// --- Diagnostics ------------------------------------------------------------
// One status line per second over USB: calibration state, CV values, and the
// CPU load average/max — the instrument for retuning kNumPartials.
//
// KEEP THIS AT 0 FOR ANYTHING YOU LISTEN TO. The once-a-second USB burst
// couples into the audio (whinebug.md), and PrintLine blocks long enough to
// starve the ADC, which makes pot reads go stale. The CPU figures include the
// logging's own load, so read them as an upper bound, not an exact cost.
//
// Also settable from the make line (`make MODULE=foxtail SERIAL_LOG=1`) so
// taking a measurement never means editing this file and then remembering to
// revert it.
#ifndef FOXTAIL_SERIAL_LOG
#define FOXTAIL_SERIAL_LOG 0
#endif

// One-shot CV null capture at startup (`make MODULE=foxtail CV_NULL=1`), for
// when only the nulls need redoing. Step 3 of the calibration gesture does the
// same thing. Boot it ONCE with nothing patched, then build normally: left on,
// it re-measures and rewrites QSPI every power-up and bakes in whatever happens
// to be patched at the time.
#ifndef FOXTAIL_CV_NULL
#define FOXTAIL_CV_NULL 0
#endif

// Coarse pitch range. Exponential map: f = kFreqMin * (kFreqMax/kFreqMin)^t, so
// the knob's centre lands on the GEOMETRIC mean — 63 Hz over 20..200, not 110.
// One decade = 3.32 octaves, deliberately narrow: this is a fundamental for an
// additive bank, and the partials, not the root, carry the top of the range.
static constexpr float kFreqMin = 20.0f;
static constexpr float kFreqMax = 200.0f;
static constexpr float kPitchOctaves = 3.32192809f; // log2(kFreqMax / kFreqMin)
static constexpr float kMaster  = 0.7f;
static constexpr float kFineDeadzone = 0.05f; // fine tune centre snap, in travel

// QUIRKS=1 gives the spectral shift pot a two-hour centre detent in software.
// A pot with a mechanical detent does not need it; this unit's has none, and
// the neutral position of a spectrum-wide control is worth being certain of.
// Two hours of a ten-hour sweep is 20% of travel, so +-0.2 in bipolar units.
#ifndef FOXTAIL_QUIRKS
#define FOXTAIL_QUIRKS 0
#endif
static constexpr float kShiftDeadzone = FOXTAIL_QUIRKS ? 0.2f : 0.0f;


// --- Calibration ------------------------------------------------------------
// Two-point (1V/3V) V/oct via libDaisy's VoctCalibration, plus the CV nulls.
// One QSPI record.
//
// To calibrate: power up with slider 1 FULLY UP and both switches away from
// their HIGH position (switch 1 up, switch 2 down) — the slider is part of the
// gesture because the switch combination alone is a normal playing position.
// The LEDs chase to confirm calibration mode, and audio runs (deliberately —
// captures must happen under playback load, see MeasureVoct).
//   1. Patch a steady 1 V into the V/OCT jack, flip switch 2 -> captures.
//   2. Patch 3 V, flip switch 2 again -> captures.
//   3. Unpatch everything, flip switch 2 again -> captures the CV nulls, saves,
//      resumes normally.
// Any other power-up state just loads the stored values.
//
// The nulls can also be captured on their own (FOXTAIL_CV_NULL), so a unit that
// only needs those does not have to redo the pitch fit.
static constexpr int kNumKnobCv  = 5;
static constexpr int kNumLevelCv = foxtail::kNumBands + 1;

struct CalData
{
    float scale;
    float offset;
    // Unpatched idle reading of each summing jack, subtracted on every read.
    // knobCvNull[0] is V/OCT and stays unused: `offset` already absorbs it.
    float knobCvNull[kNumKnobCv];
    float levelCvNull[kNumLevelCv];

    bool operator!=(const CalData& o) const
    {
        if (scale != o.scale || offset != o.offset) return true;
        for (int i = 0; i < kNumKnobCv; ++i)
            if (knobCvNull[i] != o.knobCvNull[i]) return true;
        for (int i = 0; i < kNumLevelCv; ++i)
            if (levelCvNull[i] != o.levelCvNull[i]) return true;
        return false;
    }
};

// Defaults reproduce the uncalibrated behaviour (adc * 5 octaves, no nulling).
static constexpr CalData kCalDefault = {60.0f, 0.0f, {0.f}, {0.f}};

// VoctCalibration::Record does no validation and divides by (v3 - v1): bad
// captures yield an infinite scale that would persist in QSPI. Every path that
// produces OR loads calibration data goes through this.
static bool CalSane(float scale, float offset)
{
    return std::isfinite(scale) && std::isfinite(offset) && scale > 6.0f
           && scale < 600.0f && offset > -600.0f && offset < 600.0f;
}

// A real jack null is a couple of percent. This also rejects the garbage that
// a pre-nulling QSPI image leaves in the fields appended to CalData, which
// PersistentStorage cannot detect (it has no version or checksum).
static constexpr float kCvNullMax = 0.1f;
static bool NullSane(float n)
{
    return std::isfinite(n) && n > -kCvNullMax && n < kCvNullMax;
}

PersistentStorage<CalData> calStore(hw.qspi);
float g_voctScale  = kCalDefault.scale;
float g_voctOffset = kCalDefault.offset;
float g_knobCvNull[kNumKnobCv]   = {0.f};
float g_levelCvNull[kNumLevelCv] = {0.f};

// Shaper-window LED peek: how long the row keeps showing the window after knob
// 2/3 stops moving. The timer resets on every movement.
static constexpr uint32_t kPeekMs       = 1500;
static constexpr float    kKnobDeadband = 0.004f;

Led  leds[kNumLeds];
GPIO switch1; // left  — CLUSTER / SHEPARD
GPIO switch2; // right — parity: all / odd only
foxdiag::Diag diag; // all methods compile to nothing without FOXTAIL_SERIAL_LOG

// Park the oscillator (partial state + sine table) in DTCM: uncached, zero wait
// state, no bus arbitration, so every sample costs the same. Default .bss goes
// to cached AXI SRAM, where refills burst — and bursty is what the whine feeds
// on. Verify placement after linker changes:
//   arm-none-eabi-nm build/Fox-Tail.elf | grep -w osc   -> must be 0x2000xxxx
foxtail::FoxTailOsc osc DTCM_MEM_SECTION;
foxtail::Controls   controls;

static inline float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

// Pot travel as a bipolar -1..1 with a centre deadzone, the remaining travel
// rescaled so the ends still reach full. At dz = 0 this is the identity.
static inline float Detented(float pot, float dz)
{
    const float x  = clampf(pot, 0.0f, 1.0f) * 2.0f - 1.0f;
    const float ax = fabsf(x) - dz;
    return ax <= 0.0f ? 0.0f : (x < 0.0f ? -ax : ax) / (1.0f - dz);
}

// CV jacks read a small nonzero value unpatched (~-0.02 on this unit), and it
// is subtracted here, in the only two places a CV is summed with a panel
// control. Without it the bias eats the top of the panel's travel: a knob at
// full CW reached 0.975, which is what kept CLUSTER from collapsing at max.
static inline float KnobCv(int knobIdx)
{
    return hw.GetAdcValue(kKnobCv[knobIdx]) - g_knobCvNull[knobIdx];
}

static inline float LevelCv(int idx)
{
    return hw.GetLevelCV(idx) - g_levelCvNull[idx];
}

// Panel knob summed with its CV jack. Raw on purpose: a backlash/one-pole
// conditioner was tried here and audibly clicked — it turned dense LSB noise
// into sparse larger steps. With the window taper outside the window, the raw
// noise is inaudible (hardware-verified 2026-08).
static inline float knobPlusCv(int knobIdx)
{
    return clampf(BigKnob(knobIdx) + KnobCv(knobIdx), 0.0f, 1.0f);
}

// Average the V/OCT jack over 1 s, LEDs dark. Both details matter: readings on
// this board shift with LED current and CPU load, so captures happen with audio
// running (the caller guarantees that) and the LEDs in their quietest state.
// No ProcessAllControls here — the audio callback owns control processing.
static float MeasureVoct()
{
    for (int i = 0; i < kNumLeds; ++i)
    {
        leds[i].Set(0.f);
        leds[i].Update();
    }
    System::Delay(100);

    double    acc = 0.0;
    const int kN  = 1000;
    for (int i = 0; i < kN; ++i)
    {
        acc += hw.GetAdcValue(kKnob1Cv);
        System::Delay(1);
    }
    return (float)(acc / (double)kN);
}

// Knob CVs only: the level jacks would overrun the USB TX buffer, and these are
// the four that show up in the pos/win/A/B log line.
static void PrintCvNulls()
{
    hw.PrintLine("CV nulls: knob " FLT_FMT(3) " " FLT_FMT(3) " " FLT_FMT(3) " "
                 FLT_FMT(3),
                 FLT_VAR(3, g_knobCvNull[1]), FLT_VAR(3, g_knobCvNull[2]),
                 FLT_VAR(3, g_knobCvNull[3]), FLT_VAR(3, g_knobCvNull[4]));
}

// Persist every calibrated value from its global. One writer, so a partial
// calibration can never leave QSPI holding a mix of old and new fields.
static void SaveCal()
{
    CalData& cd = calStore.GetSettings();
    cd.scale    = g_voctScale;
    cd.offset   = g_voctOffset;
    for (int i = 0; i < kNumKnobCv; ++i) cd.knobCvNull[i] = g_knobCvNull[i];
    for (int i = 0; i < kNumLevelCv; ++i) cd.levelCvNull[i] = g_levelCvNull[i];
    calStore.Save();
}

// Average every summing jack at once, same conditions as MeasureVoct. Anything
// outside kCvNullMax is a patched jack, not a null, and is left at zero, so a
// forgotten cable costs that one jack its nulling and nothing else.
static void MeasureCvNulls()
{
    for (int i = 0; i < kNumLeds; ++i)
    {
        leds[i].Set(0.f);
        leds[i].Update();
    }
    System::Delay(100);

    double    knob[kNumKnobCv]   = {0.0};
    double    level[kNumLevelCv] = {0.0};
    const int kN                 = 1000;
    for (int i = 0; i < kN; ++i)
    {
        for (int k = 0; k < kNumKnobCv; ++k) knob[k] += hw.GetAdcValue(kKnobCv[k]);
        for (int l = 0; l < kNumLevelCv; ++l) level[l] += hw.GetLevelCV(l);
        System::Delay(1);
    }

    for (int k = 0; k < kNumKnobCv; ++k)
    {
        const float n   = (float)(knob[k] / (double)kN);
        g_knobCvNull[k] = NullSane(n) ? n : 0.f;
    }
    for (int l = 0; l < kNumLevelCv; ++l)
    {
        const float n    = (float)(level[l] / (double)kN);
        g_levelCvNull[l] = NullSane(n) ? n : 0.f;
    }

    PrintCvNulls();
}

// Block until switch 2 changes state, with an LED chase showing which step we
// are on (step+1 lamps lit solid).
static void WaitForCapture(int step)
{
    const bool start = switch2.Read();
    uint32_t   t     = 0;
    while (switch2.Read() == start)
    {
        uint32_t now = System::GetNow();
        if (now - t >= 60)
        {
            t = now;
            const int lit = (int)((now / 120) % (uint32_t)kNumLeds);
            for (int i = 0; i < kNumLeds; i++)
                leds[i].Set((i <= step) ? 1.f : (i == lit ? 0.15f : 0.f));
        }
        for (int i = 0; i < kNumLeds; i++) leds[i].Update();
        System::Delay(1);
    }
}

// Blocking three-step calibration; audio is already running.
static void RunCalibration()
{
    hw.PrintLine("V/oct calibration: patch 1V, flip switch 2.");

    float v[2] = {0.f, 0.f};
    for (int step = 0; step < 2; ++step)
    {
        WaitForCapture(step);
        v[step] = MeasureVoct();
        hw.PrintLine("  captured %dV -> " FLT_FMT(5), step ? 3 : 1, FLT_VAR(5, v[step]));
        if (step == 0) hw.PrintLine("Now patch 3V and flip switch 2 again.");
    }

    // Step 3, the CV nulls. Last because it is the only step that wants the
    // patch cables OUT, and because a rejected pitch fit must not cost it.
    hw.PrintLine("Now UNPATCH every jack and flip switch 2 to capture CV nulls.");
    WaitForCapture(2);
    MeasureCvNulls();

    float scale = 0.f, offset = 0.f;
    if (std::fabs(v[1] - v[0]) > 0.02f)
    {
        VoctCalibration cal;
        cal.Record(v[0], v[1]);
        cal.GetData(scale, offset);
    }

    if (!CalSane(scale, offset))
    {
        // Refuse to store nonsense; keep the previous pitch values and flash
        // alternating lamps so the failure is unmistakable. The nulls are
        // independent of the fit, so they are still worth keeping.
        hw.PrintLine("calibration REJECTED (readings too close or fit out of "
                     "range) — keeping previous values, nulls saved");
        SaveCal();
        for (int n = 0; n < 6; n++)
        {
            for (int i = 0; i < kNumLeds; i++)
            {
                leds[i].Set(((i + n) & 1) ? 1.f : 0.f);
                leds[i].Update();
            }
            System::Delay(150);
        }
        return;
    }

    g_voctScale  = scale;
    g_voctOffset = offset;
    SaveCal();

    hw.PrintLine("saved: scale=" FLT_FMT(4) " offset=" FLT_FMT(4),
                 FLT_VAR(4, g_voctScale), FLT_VAR(4, g_voctOffset));

    for (int i = 0; i < kNumLeds; i++) { leds[i].Set(1.f); leds[i].Update(); }
    System::Delay(600);
}

// Audio callback: fill the seam from hardware, then render. Input `in` is
// intentionally ignored (noise budget). Non-interleaved out[ch][frame].
void audioCallback(AudioHandle::InputBuffer  /*in*/,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    diag.BlockStart();

    hw.ProcessAllControls();

    controls.inharm = clampf(Slider(0) + LevelCv(0), 0.0f, 1.0f);
    controls.master = kMaster;

    // Pot 1 -> spectral shift, under slider 1's inharmonicity: both bend the
    // whole spectrum. Pot 2 -> fine tune, under the fundamental's own band.
    // Pot 3 -> stereo spread.
    controls.bandShift = 0.5f + 0.5f * Detented(PanPot(0), kShiftDeadzone);
    controls.fineTune  = Detented(PanPot(1), kFineDeadzone);
    controls.spread    = clampf(PanPot(2), 0.0f, 1.0f);

    for (int b = 0; b < foxtail::kNumBands; ++b)
    {
        const int s = b + kBandOffset;
        controls.bandGain[b] = clampf(Slider(s) + LevelCv(s), 0.0f, 1.0f);
    }
    // Pots 4-9 shape bands 2-7. Bands 0-1 hold two partials each, so there is
    // no fill order to choose; they keep the Controls default of 0 (lowpass).
    for (int b = kFirstShapeBand; b < foxtail::kNumBands; ++b)
        controls.bandShape[b] = clampf(PanPot(b + 1), 0.0f, 1.0f);

    // Knob 1: pitch, exponential sweep (constant octaves per degree of
    // rotation, so the detent sits at the geometric mean of the range, not the
    // arithmetic one). V/OCT jack adds octaves through the stored calibration
    // (scale/offset are in semitones, hence /12).
    float t          = clampf(BigKnob(0), 0.0f, 1.0f);
    // exp2f, not powf: the base is constant, so b^t == exp2(t*log2(b)) exactly,
    // and powf is the most expensive call in the block path (it is exp(t*log(b))
    // internally, with the log recomputed every callback).
    controls.pitchHz = kFreqMin * exp2f(t * kPitchOctaves);
    controls.pitchCv = (g_voctScale * hw.GetAdcValue(kKnob1Cv) + g_voctOffset) * (1.0f / 12.0f);

    controls.position = knobPlusCv(1);
    controls.window   = knobPlusCv(2);
    controls.shapeA   = knobPlusCv(3);
    controls.shapeB   = knobPlusCv(4);

    // Measured polarity: switch 1 reads HIGH when down, switch 2 HIGH when up.
    controls.mode   = switch1.Read() ? foxtail::kModeCluster : foxtail::kModeShepard;
    controls.parity = switch2.Read() ? 0.0f : 1.0f;

    // out[0] is the physical RIGHT jack: the output pair is crossed below
    // libDaisy (the delay compensates for the same swap inside panToVolume() in
    // dsp.h instead). Correcting it here, at the one hardware seam, keeps
    // "pan = 1 means right" true in the engine, the emulator, and anything
    // stereo added later.
    osc.Process(controls, out[1], out[0], size);

    diag.BlockEnd(out[0], out[1], size);
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

    // Logging first: calibration talks you through the procedure over serial.
    hw.StartLog(false); // non-blocking, runs with or without a monitor

    // Load stored calibration; fall back to defaults if the stored data is
    // invalid so a bad save can never brick the pitch path.
    calStore.Init(kCalDefault);
    {
        const CalData& cd = calStore.GetSettings();
        if (CalSane(cd.scale, cd.offset))
        {
            g_voctScale  = cd.scale;
            g_voctOffset = cd.offset;
            hw.PrintLine("V/oct: scale=" FLT_FMT(4) " offset=" FLT_FMT(4) " [%s]",
                         FLT_VAR(4, g_voctScale), FLT_VAR(4, g_voctOffset),
                         calStore.GetState() == PersistentStorage<CalData>::State::USER
                             ? "USER: calibrated"
                             : "FACTORY: defaults, never calibrated");
        }
        else
        {
            hw.PrintLine("stored V/oct calibration is invalid — using defaults");
            g_voctScale  = kCalDefault.scale;
            g_voctOffset = kCalDefault.offset;
        }

        // Per field, not all-or-nothing: a QSPI image saved before the nulls
        // existed has a valid pitch fit followed by whatever bytes happened to
        // be there, and those must not reach the control seam.
        for (int i = 0; i < kNumKnobCv; ++i)
            g_knobCvNull[i] = NullSane(cd.knobCvNull[i]) ? cd.knobCvNull[i] : 0.f;
        for (int i = 0; i < kNumLevelCv; ++i)
            g_levelCvNull[i] = NullSane(cd.levelCvNull[i]) ? cd.levelCvNull[i] : 0.f;
        PrintCvNulls();
    }

    osc.Init(hw.AudioSampleRate());

#if FOXTAIL_SERIAL_LOG
    diag.Init(hw.AudioSampleRate(), 5);
#endif

    hw.StartAudio(audioCallback);

    // Captures run AFTER StartAudio so they happen under the same electrical
    // conditions as playback (readings shift with load, see above).
    System::Delay(150); // let the callback's control processing settle

#if FOXTAIL_CV_NULL
    MeasureCvNulls();
    SaveCal();
#endif

    if (!switch1.Read() && !switch2.Read() && Slider(0) > 0.9f)
        RunCalibration();

    // Drive LEDs from the main loop (not the audio IRQ) to avoid FPU
    // lazy-stacking corruption of Led::Update()'s pwm accumulator. Everything
    // below is main-loop only and costs the audio thread nothing.
    uint32_t last_led_ms = 0;
    uint32_t last_log_ms = 0;
    uint32_t peek_until  = 0;
    float    last_pos = -1.f, last_win = -1.f;
    (void)last_log_ms;

    while (true)
    {
        diag.MainLoopTick();
        uint32_t now = System::GetNow();
        if (now - last_led_ms >= 1)
        {
            // Knobs 2 and 3 set the shaper window, which is otherwise invisible
            // on the module. Touching either flips the LED row to a window view
            // for kPeekMs after the knob stops moving.
            const float pos = controls.position;
            const float win = controls.window;
            if (std::fabs(pos - last_pos) > kKnobDeadband
                || std::fabs(win - last_win) > kKnobDeadband)
            {
                peek_until = now + kPeekMs;
                last_pos   = pos;
                last_win   = win;
            }

            leds[0].Set(clampf(controls.inharm, 0.f, 1.f));

            if (now < peek_until)
            {
                // Window view: each LED shows how much of its band the window
                // covers, so the edges come out partially lit.
                const float bs = osc.BandOfRatio(osc.WindowStart());
                const float be = osc.BandOfRatio(osc.WindowEnd());
                for (int b = 0; b < foxtail::kNumBands; b++)
                {
                    const float lo = (float)b - 0.5f, hi = (float)b + 0.5f;
                    const float ov = std::fmin(be, hi) - std::fmax(bs, lo);
                    leds[b + kBandOffset].Set(clampf(ov, 0.f, 1.f));
                }
            }
            else
            {
                // Level view. A band whose slider is up but whose partials have
                // all run past Nyquist pulses dimly instead of going dark —
                // otherwise "turned down" and "nothing left to play" look alike.
                const float pulse = 0.10f + 0.10f * std::sin((float)now * 0.006f);
                for (int b = 0; b < foxtail::kNumBands; b++)
                {
                    const float m       = osc.Meter(b);
                    const bool  starved = m > 0.05f && !osc.Audible(b);
                    leds[b + kBandOffset].Set(starved ? pulse : m);
                }
            }
            for (int i = 0; i < kNumLeds; i++) leds[i].Update();
            last_led_ms = now;
        }

#if FOXTAIL_SERIAL_LOG
        if (now - last_log_ms >= 1000)
        {
            // One compact line per second (longer output overruns the USB TX
            // buffer and truncates lines).
            //   cv[..] = the six direct CV inputs (spread time fb hpf lpf dry),
            //            RAW: the stored nulls are not subtracted here, so a
            //            calibrated module still shows the idle bias. Rails at
            //            ~0.99 on USB-only power.
            //   voct   = raw TIME_CV / pitch CV in octaves. Tracking check:
            //            1 V -> 3 V patched must step the octaves by 2.000.
            //   cpu    = avg/max %. Retune kNumPartials against the WORST-CASE
            //            mode (Cluster) and keep real headroom: overload starves
            //            the ADC, USB and LEDs, not just the audio.
            // A second line alternates with it (one line would truncate),
            // printing the control seam as the engine receives it. pos/win/A/B
            // are knob PLUS CV jack — A/B against the emulator with these
            // numbers, not with physical knob angles.
            static int logAlt = 0;
            logAlt = (logAlt + 1) % 3;
            if (logAlt == 1)
            {
                hw.PrintLine("pot1=" FLT_FMT(3) " fine=" FLT_FMT(3)
                             " pos=" FLT_FMT(3) " win=" FLT_FMT(3)
                             " A=" FLT_FMT(3) " B=" FLT_FMT(3)
                             " f0=" FLT_FMT(1),
                             FLT_VAR(3, PanPot(0)),
                             FLT_VAR(3, controls.fineTune),
                             FLT_VAR(3, controls.position),
                             FLT_VAR(3, controls.window),
                             FLT_VAR(3, controls.shapeA),
                             FLT_VAR(3, controls.shapeB),
                             // Effective f0, not pitchHz: fine tune and V/oct
                             // fold in here, and pitchHz alone hides both.
                             FLT_VAR(1, controls.pitchHz
                                            * exp2f(controls.pitchCv
                                                    + controls.fineTune
                                                          * (1.0f / 12.0f))));
            }
            else if (logAlt == 2)
            {
                hw.PrintLine("p=%d s%d%d cv[" FLT_FMT(2) " " FLT_FMT(2) " "
                             FLT_FMT(2) " " FLT_FMT(2) " " FLT_FMT(2) " "
                             FLT_FMT(2) "] voct=" FLT_FMT(3) "/" FLT_FMT(2)
                             " cpu=" FLT_FMT(1) "/" FLT_FMT(1),
                             foxtail::kNumPartials,
                             switch1.Read() ? 1 : 0,
                             switch2.Read() ? 1 : 0,
                             FLT_VAR(2, hw.GetAdcValue(SPREAD_CV)),
                             FLT_VAR(2, hw.GetAdcValue(TIME_CV)),
                             FLT_VAR(2, hw.GetAdcValue(FEEDBACK_CV)),
                             FLT_VAR(2, hw.GetAdcValue(HIGHPASS_CV)),
                             FLT_VAR(2, hw.GetAdcValue(LOWPASS_CV)),
                             FLT_VAR(2, hw.GetAdcValue(LEVEL_DRY_CV)),
                             FLT_VAR(3, hw.GetAdcValue(kKnob1Cv)),
                             FLT_VAR(2, controls.pitchCv),
                             FLT_VAR(1, diag.AvgCpu()),
                             FLT_VAR(1, diag.MaxCpu()));
            }
            else
            {
                // Health line, all counters per window. Field key and the
                // reasoning behind each counter: foxtail_diag.h.
                diag.PrintStatus(hw);
            }
            last_log_ms = now;
        }
#endif
    }
}
