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
- `make` produces `build/Tap-O-Matic.bin`; `make MODULE=foxtail` produces
  `build/Fox-Tail.bin`. Flash via `make program-dfu` or https://flash.daisy.audio
  (hold BOOT, tap RESET to enter DFU).
- The user builds and flashes themselves; don't run `make` for them.

## The whine (investigated, closed)

The module emits tonal noise caused by bursty digital activity coupling into
the audio path. Fully diagnosed and attributed in
`docs/claude/whinebug.md` — read it before touching anything
noise-related. Executive summary: the delay firmware is already near its
psychoacoustic optimum (do NOT re-attempt round-robin control scanning — it
was tested and sounds worse); the excess coupling vs the original OAM Time
Machine is a board-level hardware issue; a lightweight firmware reaches the
hardware floor. Fox Tail's residual ~9.6 kHz tone is the callback-rate spur
whinebug predicted as unavoidable, not a new problem.

## Fox Tail — additive oscillator firmware + emulator

Second firmware: a 96-partial additive sine oscillator, hardware-verified end
to end. Docs: `docs/claude/oscillator-impl.md` (repo architecture),
`docs/claude/control-maps.md` (control map, engine, measured facts — the living
doc), `docs/claude/pigments-harmonic-engine.md` (commercial reference). Obeys
the whinebug noise budget: no external RAM, constant work per callback, block
size 5, never read the audio input.

**Three layers** (the key idea — keep sources out of firmware):
- `foxtail_dsp.h` (repo root): the shared, hardware-free engine. **Must stay
  C++14-safe** (firmware is `-std=gnu++14`; no `std::clamp`/`std::optional`).
  `Controls` is the seam; `kNumPartials = 96` is the CPU dial (budget against
  Cluster, the worst-case mode — see control-maps.md before changing it).
- `FoxTail.cpp` (repo root): thin hardware shell — controls → `Controls`,
  V/oct calibration, LED views, serial diagnostics. Osc object lives in DTCM.
- `emulator/`: `./emulator/run.sh` → http://localhost:4343. Runs the identical
  engine at the firmware's 5-frame block size — never weaken that equivalence.
  `emulator/src/sources.h` = emulator-only CV sources (env/LFO stand-ins for
  patched modules); the firmware never includes it. Web UI JS is split by theme
  (user prefers small, focused files — keep it that way): `app.js`, `sources.js`
  (ranges in `RANGES`), `labels.js`, `partials.js` (viewer draws the engine's
  own `/partials` state, computes nothing).

**Control map** (details + rationale in control-maps.md): slider 1 =
inharmonicity (+CV), pot 1 = fine tune ±1 semitone (centre deadzone in
firmware); sliders/pots 2–9 = gain/pan of 8
geometric bands; knob 1 = pitch (jack is calibrated V/oct); knobs 2–5 = shaper
(window start/width, then per-mode params); switch 1 down = CLUSTER / up =
SHEPARD; switch 2 up = ALL / down = ODD ONLY. GATE (digital-only jack) is still
unassigned. Measured quirks: switch 1 reads HIGH down, switch 2 HIGH up; pot 1
reads opposite polarity to pots 2–9.

**Hard rules learned on this hardware** (mechanisms in control-maps.md):
- No libm/`std::sqrt`, no FP ternaries, no branches in per-partial paths.
  Foxtail builds `-O3 -fno-math-errno -fno-trapping-math` (delay stays `-Os`).
- Keep CPU headroom: overload starves the ADC, USB and LEDs, not just audio.
  Check cost with objdump, not estimates — the per-BLOCK control pass dominates,
  not the render loop.
- **USB-only power leaves the SM's analog domain dead**: CV inputs rail at
  ~0.99, audio silent, while pots/sliders keep working. Bench-test CVs and
  audio only with rack power (USB may stay attached for serial).

**V/oct calibration:** two-point 1V/3V, stored in QSPI. Power up with slider 1
FULLY UP + switch 1 up + switch 2 down; flip switch 2 to capture each point.
Runs with audio live and LEDs dark so captures match playback conditions
(readings shift with load on this board). All produce/load paths go through
`CalSane()` because `VoctCalibration::Record` divides by (v3−v1) unvalidated.
Result on this unit: tracking within a few cents over 2 octaves.

**Diagnostics:** `FOXTAIL_LED_MODE` in FoxTail.cpp (0 = normal + shaper-window
peek, 1 = CPU bar, 2 = switch states); `FOXTAIL_SERIAL_LOG` prints one status
line/s (CV values, voct raw/octaves, cpu avg/max). Tracking check: 1 V → 3 V
patched must step the voct octaves by exactly 2.000.

**Panel labels.** The SVG (`panel/Fox-Tail.svg`) is the source of truth.
`emulator/controls.json` maps control id → `<tspan>` id. Edit labels live in the
emulator UI (writes the SVG + re-renders). Regenerate the PNG with
`python3 emulator/render_panel.py` (rsvg-convert + Futura font remap + autocrop);
**never hand-edit `emulator/web/Fox-Tail.png`**. Emulator control state persists
in the browser's localStorage.

**Open:** GATE assignment; listening-driven tuning of ranges/curves; panel
relabeling once the map feels final (labels still show delay-era names);
optional switch-direction swaps before labels are cut.