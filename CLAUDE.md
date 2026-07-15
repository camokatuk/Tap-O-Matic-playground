# Tap-O-Matic playground

DIY Eurorack multi-tap delay ("Time Machine" lineage) on an Electrosmith
Daisy Patch SM (STM32H750). This fork is a playground for firmware
experiments; the stock delay firmware is `TimeMachine.cpp` +
`time_machine_hardware.{h,cpp}` (board support: ADC muxing, calibration, LEDs).

## Building

- Toolchain: `arm-none-eabi-gcc` (Homebrew cask `gcc-arm-embedded`) + `dfu-util`.
- Libraries expected at `../DaisyExamples/` (libDaisy + DaisySP), built via
  `ci/build_libs.sh`.
- **Gotcha:** `DaisySP-LGPL` is a *nested* git submodule that
  `git clone --recurse-submodules` of DaisyExamples may miss. If the build
  fails with `daisysp-lgpl.h: No such file or directory`, run
  `git submodule update --init --recursive` inside `DaisyExamples/DaisySP`,
  then `make` inside `DaisySP/DaisySP-LGPL`.
- `make` produces `build/Tap-O-Matic.bin`; flash via `make program-dfu` or
  https://flash.daisy.audio (hold BOOT, tap RESET to enter DFU).
- The user builds and flashes themselves; don't run `make` for them.

## The whine (investigated, closed)

The module emits tonal noise caused by bursty digital activity coupling into
the audio path. Fully diagnosed and attributed in
`docs/claude/whinebug.md` — read it before touching anything
noise-related. Executive summary: the delay firmware is already near its
psychoacoustic optimum (do NOT re-attempt round-robin control scanning — it
was tested and sounds worse); the excess coupling vs the original OAM Time
Machine is a board-level hardware issue; a lightweight firmware reaches the
hardware floor.

## NEXT UP: additive sine oscillator firmware (planned, not started)

Agreed plan: build an **alternative firmware** turning the module into an
additive sine-wave oscillator, as a separate build target (e.g.
`AdditiveOsc.cpp` + Makefile target) reusing `time_machine_hardware.*`
untouched, so the delay firmware stays intact and both remain flashable.

Noise budget (from whinebug.md, all empirically validated):
- No external-RAM usage at all.
- Constant DSP work per callback; no periodic structure below the callback
  rate.
- Callback-rate spur placed high (block size ≤ 5 → ≥ 9.6 kHz).
- Don't process/pass the audio input.

**Proposed control mapping — NOT yet reviewed with the user; discuss and
agree on functionality/controls before writing code:**

| Hardware | Proposed role |
|---|---|
| 8 sliders + their CV ins | Amplitudes of harmonics 1–8 |
| 9th slider (dry) + CV | Master level? (TBD) |
| Time knob + Time CV | Pitch: coarse tune + 1V/oct tracking |
| Spread knob + CV | Harmonic stretch/detune (organ → inharmonic/bell) |
| Feedback knob + CV | TBD (e.g. global FM, drift, or wavefolding) |
| Highpass/Lowpass knobs + CVs | Spectral tilt / brightness (TBD) |
| 8 small pan pots | Per-harmonic stereo placement or fine detune (TBD) |
| 2 switches | TBD (octave range, mono/stereo, quantize?) |
| Gate/clock in | Sync or trigger (TBD) |
| 9 LEDs | Per-harmonic level meters |
| Audio out L/R | Oscillator output |

Open questions to settle with the user first: exact role of feedback/filter
knobs, pan pots, switches, and the dry slider; whether pitch CV needs the
existing calibration system; mono-vs-stereo philosophy.
