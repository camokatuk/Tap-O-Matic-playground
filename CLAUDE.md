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

## Fox Tail — additive oscillator firmware + emulator (in progress)

Second firmware turning the module into an additive sine oscillator. Full
living doc: `docs/claude/oscillator-impl.md`. Must obey the noise budget
(see whinebug.md): no external RAM, constant work per callback, block size ≤ 5,
never read the audio input.

**Build.** `make MODULE=foxtail` → `Fox-Tail.bin` (delay is default `make`).
User builds/flashes firmware themselves.

**Three-layer architecture** (the key idea — keep sources out of firmware):
- `foxtail_dsp.h` (repo root): the shared, hardware-free DSP. **Must stay
  C++14-safe** (firmware is `-std=gnu++14`; no `std::clamp`/`std::optional`).
  `Controls` struct is the seam. `kNumHarmonics = 9` (9 sliders = 9 partials).
- `FoxTail.cpp` (repo root): thin firmware — reads `time_machine_hardware.*`
  into `Controls`, no source generators. Slider `h` = harmonic `h`
  (`amp = clamp(slider + levelCV, 0..1)`); TIME knob = base pitch; V/OCT jack
  (`TIME_CV`, analog) = pitch CV in octaves.
- `emulator/` (native audio + web UI, **emulator-only sources**): run with
  `./emulator/run.sh` → http://localhost:4343 (miniaudio→CoreAudio + cpp-httplib;
  builds via clang++ with no cmake needed). `emulator/src/sources.h` =
  `Envelope`/`Oscillator` (dev stand-ins for patched modules); the firmware
  never includes it. Web UI JS is split by theme (user prefers small, focused
  files — keep it that way): `app.js` (core glue + panel controls +
  persistence), `sources.js` (CV source cards; all slider ranges live in its
  `RANGES` object), `labels.js` (label model + editor); shared via a small `FT`
  namespace + global `send`/`saveVal`/`span`/`rowEl`.

**Modulation.** General `CvSource` per target (pitch + each slider): off/osc/env
+ depth. Slider CV sums into slider (clamped); pitch CV is octaves (`2^cv`). Osc
is a bipolar sine LFO. Jack facts: analog CV jacks = TIME/SPREAD/FEEDBACK/HPF/LPF
(pitch on TIME); **GATE jack is digital** (can't do analog CV).

**Panel labels.** The SVG (`panel/Fox-Tail.svg`) is the source of truth.
`emulator/controls.json` maps control id → `<tspan>` id. Edit labels live in the
emulator UI (writes the SVG + re-renders). Regenerate the PNG with
`python3 emulator/render_panel.py` (rsvg-convert + Futura font remap + autocrop);
**never hand-edit `emulator/web/Fox-Tail.png`**. Emulator control state persists
in the browser's localStorage.

**Done:** additive osc (basic `std::sin` per partial, unoptimized), env+osc
sources wired to pitch and to every slider CV (emulator + firmware).
**Not done / open:** spread/feedback/hpf/lpf/GATE have no DSP function yet;
final control map; osc CPU optimization; the new firmware paths are untested on
hardware.
