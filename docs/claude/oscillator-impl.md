# Fox Tail oscillator — implementation doc

Living document for the **Fox Tail** additive-sine-oscillator firmware, a second
firmware for the same Daisy Patch SM hardware that today runs the **Tap-O-Matic**
delay. Plan first, then the task list (todos + done). Update as we go.

Naming: the delay firmware stays **Tap-O-Matic**; the oscillator firmware is
**Fox Tail**. Shared board support stays `time_machine_hardware.*`.

Read first: `docs/claude/whinebug.md` (the noise budget every new firmware must
respect) and `CLAUDE.md` (project constraints, proposed control mapping).

---

## 1. Plan

### 1.1 Goals

1. Turn the module into an additive sine oscillator (harmonics 1–8 on the eight
   sliders) **without touching the working Tap-O-Matic delay** — both firmwares
   stay flashable.
2. Make firmware selection a **single swap point** so trying a different module
   is one command, not a code edit.
3. Make the *new* firmware **desktop-testable** (an emulator harness) so we can
   iterate on sound and controls without flashing hardware every time.

### 1.2 What is "the module" today, and where the swap point is

`TimeMachine.cpp` (745 lines) is the entire delay **application**: hardware
bootstrap, calibration + `PersistentStorage`, delay-specific control mapping
(`updateControlHandlers`), the `audioCallback`, and the `main()` loop. The DSP
engine lives in `dsp.h` (`StereoTimeMachine`). Both are delay-specific.

`time_machine_hardware.{h,cpp}` is the reusable board-support layer (ADC mux,
LEDs, knob/slider/CV accessors, calibration plumbing). It is firmware-agnostic
and stays untouched.

**Key realization:** the delay app is referenced in exactly one place — the
`CPP_SOURCES` line of the Makefile. So the swap point already almost existed;
we just made it explicit.

### 1.3 Pluggability decision — chosen approach: Makefile module selector

**Done.** Each firmware is one application `.cpp` with its own `main()`, sharing
the board-support layer. The Makefile picks one via `MODULE=`:

```
make                  -> Tap-O-Matic delay     (Tap-O-Matic.bin)  [default]
make MODULE=foxtail   -> Fox Tail oscillator    (Fox-Tail.bin)     [once FoxTail.cpp exists]
```

Why this over a shared `main()` + abstract `IModule` base class:

- **Zero risk to the delay.** A bare `make` still compiles the identical
  `time_machine_hardware.cpp TimeMachine.cpp`. Nothing about Tap-O-Matic moves.
- **The two bootstraps genuinely differ.** The delay needs calibration,
  `PersistentStorage`, and a 150 s SDRAM buffer; the oscillator's noise budget
  forbids the SDRAM buffer entirely and it needs none of the calibration path.
  A shared `main()` would be mostly `if (delay) … else …`, i.e. not shared.
- **Swapping is one token** (`MODULE=foxtail`), which satisfies the
  "replace the one place it's referenced" goal.

A shared abstract `IModule` interface is *not* worth it for the firmware itself.
But an abstraction **is** worth it for one narrower purpose — see 1.4.

### 1.4 Emulator-friendliness decision — abstract the hardware, only for Fox Tail

The Fox Tail firmware is new code, so we can write it against a thin
**hardware-access interface** instead of calling `hw.GetLevelSlider(...)` etc.
directly. That single seam is what lets the *same* DSP source compile two ways:

- **On the Daisy:** the interface is backed by the real `TimeMachineHardware`.
- **On desktop:** the interface is backed by a mock fed from a GUI, with audio
  going to the sound card.

Sketch (names TBD):

```
struct Controls {            // what Fox Tail reads each block
    float harmonic[8];       // sliders 1..8  (+ CV summed in)
    float pan[8];            // 8 small pots
    float pitch, spread, feedback, lowpass, highpass;  // 5 big knobs (+CV)
    float master;            // dry slider / master level
    bool  sw1, sw2;
    bool  gate;
};
```

Fox Tail's core becomes `FoxTailOsc::Process(const Controls&, float* outL, float* outR)`
plus `FoxTailOsc::Levels(float out[8])` for the LED meters — pure DSP, no
libDaisy. `FoxTail.cpp` (the firmware `main`) reads `TimeMachineHardware`, fills
`Controls`, and calls it. The desktop harness fills the same struct from sliders.

The delay is **not** refactored this way — it stays hardware-coupled. Only Fox
Tail pays the (small) cost of the seam, and only Fox Tail gets the emulator.

### 1.5 Control mapping — still to settle with the user

Per `CLAUDE.md`, the mapping table is proposed but **not agreed**. Open
questions to resolve before writing DSP:

- Feedback knob role: global FM? drift? wavefolding?
- Highpass/Lowpass knobs: spectral tilt / brightness — exact behavior?
- 8 pan pots: per-harmonic stereo placement vs. per-harmonic fine detune?
- Dry slider: master level (assumed) — confirm.
- 2 switches: octave range / mono-stereo / quantize?
- Gate/clock in: hard sync vs. trigger?
- Pitch CV: does it need the existing calibration system, or is 1V/oct
  good enough uncalibrated for a first pass?
- Mono vs. stereo philosophy.

These are gated on a conversation, not code. Task list keeps them as a blocker.

### 1.6 Noise budget (hard constraints from whinebug.md)

- No external-RAM (SDRAM) usage at all.
- Constant DSP work per callback — no data-dependent branches in the hot loop,
  no periodic structure slower than the callback rate.
- Keep block size ≤ 5 (callback-rate spur ≥ 9.6 kHz, least audible).
- Don't read/pass the audio input.

Fox Tail meets all four by construction (fixed 8-partial sum, no buffer, no
input). This is exactly the "lightweight firmware reaches the hardware floor"
case from the whine investigation.

---

## 2. Task list

### Done

- [x] **Makefile module selector.** `MODULE=tapomatic` (default) / `MODULE=foxtail`.
      Delay build is unchanged; `FoxTail.cpp` referenced but not yet created.
- [x] **This implementation doc** (plan + tasks).
- [x] **Stack decided:** native audio + web UI (see 3.1).
- [x] **Shared DSP seam** `foxtail_dsp.h` (repo root): `foxtail::Controls` +
      `FoxTailOsc`, hardware-free, additive sine (v1 uses harmonic 0). Included
      by both emulator and (later) firmware.
- [x] **Emulator scaffold** in `emulator/` (fully separate from firmware):
      `src/main.cpp` (miniaudio→CoreAudio + cpp-httplib on :4343, lock-free
      atomics), `web/` (panel.png background + overlaid DRY slider & TIME knob,
      host master-volume strip, hover status bar, LED meter poll),
      vendored `miniaudio.h` + `httplib.h`, `CMakeLists.txt`, `run.sh`
      (CMake if present, else direct clang++ — no installs needed).
- [x] **Verified on this Mac:** compiles clean (clang++ -std=c++17), CoreAudio
      opens at 48 kHz, UI + panel.png serve, control hit `amp0` 0.5→0.9 flows
      through the DSP and back as a meter value (audio callback confirmed live).

- [x] **`FoxTail.cpp` firmware** (repo root): thin bootstrap — no SDRAM, no
      calibration, no input passthrough, block size 5, LEDs from the main loop.
      Fills the `Controls` seam from `TimeMachineHardware` (DRY slider → amp,
      TIME knob → 20–2000 Hz log) and runs the shared `FoxTailOsc`. Verified:
      board-support compiles standalone (no delay headers), ARM toolchain
      present. Builds with `make MODULE=foxtail` → `build/Fox-Tail.bin`.
      *Not yet flash-tested on hardware.*

### Milestone 1 status

DRY slider → amplitude, TIME knob → frequency of one sine: **built end-to-end in
both the emulator (verified running) and the firmware (compiles; awaiting a
hardware flash-test).** Next: user flash-tests `Fox-Tail.bin` and opens the
emulator UI; once both feel right we (a) settle the full control map (§1.5) and
(b) grow the DSP toward it. Note: emulator TIME law and firmware TIME law are
kept identical (20–2000 Hz log) so the two agree.

### Todo — firmware

- [ ] **Settle the control mapping with the user** (section 1.5). *Blocks DSP.*
- [ ] Define the `Controls` struct + hardware-access seam (section 1.4).
- [ ] `FoxTailOsc` core DSP: 8 additive sine partials, phase accumulators,
      spread/stretch, master level, per-partial pan. Pure, no libDaisy.
- [ ] `FoxTail.cpp` firmware `main`: bootstrap (no SDRAM, no calibration),
      fill `Controls` from `TimeMachineHardware`, drive audio callback, LED
      level meters in the main loop.
- [ ] Verify noise budget on hardware (block size, constant work, no input).
- [ ] Decide pitch calibration approach (reuse `PersistentStorage`? or skip).

### Todo — emulator / test harness

- [ ] **Build the desktop test harness** (see section 3 for the analysis).
      Reuses `FoxTailOsc::Process` unchanged; GUI provides the `Controls`.
- [ ] Panel-accurate GUI: 8 sliders + 8 CV inputs above, 8 small pots below,
      5 big knobs, 2 switches, gate, other outputs — laid out as a "module"
      block, with extra host controls (master volume, input source selectors,
      an implementation picker) styled separately.
- [ ] Module picker so the harness can host multiple implementations as we add
      them.

### Open questions / parking lot

- Pitch CV calibration (see above).
- Whether the delay should *ever* get the same hardware seam (only if we later
  want it in the emulator too — currently no).

---

## 3. Emulator — is it hard? (analysis, not yet started)

**Short answer: not hard, and most of it is boilerplate — *provided* Fox Tail's
DSP is written against the hardware seam (section 1.4). The audio DSP is not
custom to the emulator; we run the exact same `FoxTailOsc::Process` the firmware
runs.**

### Is there a ready-made Daisy emulator?

Not really — nothing drop-in. libDaisy is bare-metal STM32H7 (ST HAL,
interrupt-driven SAI audio, hardware ADC mux); it does not build or run on a
desktop, and there is no official Electrosmith desktop simulator. Community
efforts to run Daisy firmware on the host exist but are partial and not worth
adopting.

The practical route is **not** to emulate the STM32. It's to keep the DSP free
of hardware calls (the seam) and run *that* on desktop with a normal audio
backend. We emulate the *panel and I/O*, not the chip. This is the standard way
Daisy devs prototype, and it's why the seam is the only real prerequisite.

### Recommended stack: native C++ (miniaudio + Dear ImGui)

- **Audio:** `miniaudio` (single header) or PortAudio/RtAudio — opens the
  default output device and calls a callback. Inside the callback we call
  `FoxTailOsc::Process` block by block. **No custom audio DSP** beyond a thin
  bridge; the synthesis is the real firmware code.
- **GUI:** Dear ImGui (immediate-mode, trivial sliders/knobs/toggles). Lay the
  8 sliders with 8 CV inputs above, 8 pots below, 5 knobs, 2 switches, gate as a
  "module" panel; put host-only controls (master volume, CV/gate source
  selectors, module picker) in a separate styled strip.
- **Build:** a small separate CMake project under e.g. `emulator/` that compiles
  the shared pure-DSP sources plus the harness. Kept out of the Daisy Makefile
  so it never touches the firmware build.

Effort estimate: a few hundred lines, mostly ImGui layout and a param→`Controls`
mapping. The risky/interesting part (the actual sound) is shared with the
firmware, so the emulator can't drift from the hardware behavior.

### Alternative: Web (WASM) — nicer to share, more plumbing

Compile `FoxTailOsc` to WebAssembly (Emscripten), run it in a Web Audio
`AudioWorklet`, GUI in HTML/JS. Pros: shareable link, no install, easy to make
pretty. Cons: Emscripten toolchain, AudioWorklet + SharedArrayBuffer param
bridging, a second build path. Same core code reused. Good as a *later* "show
someone" build; native is the better day-to-day dev tool.

**Recommendation:** native miniaudio + ImGui first (fastest iteration, runs the
real code, real low-latency audio). Consider a WASM build later if we want to
share it. The whole thing hinges on the section 1.4 seam — which we should put
in from Fox Tail's first line of DSP.

### 3.1 DECIDED (2026-07-22): native audio + web UI

After discussion the chosen stack is **native audio + web UI** (not all-web,
not all-native ImGui). Reasons the user's constraints selected it: audio is the
priority, they want a UI they can read/edit (HTML/CSS), they don't have/ want
Emscripten, and they want "one command → open a browser tab."

**Crucial clarification that drove the choice:** none of the options stream
audio over the network. In this option the DSP runs **natively to CoreAudio**
(best latency/quality); the browser carries **only control changes** (knob/
slider/trigger), sent to the native app over localhost. So there is *no* audio
penalty from using a web UI here — the web layer never touches audio.

Architecture:

```
[ C++ app  foxtail-emu ]
   miniaudio -> CoreAudio        (real foxtail::FoxTailOsc DSP, native latency)
   embedded HTTP server (cpp-httplib) on localhost:4343
        |  ^
   control POSTs (id=..&v=..)  |  meter poll (GET /meters)
        v  |
[ browser tab ]  panel.png background + HTML/CSS controls overlaid + status bar
```

- Audio: **miniaudio** (single-header, MIT/PD) → CoreAudio, 48 kHz stereo f32.
- Server: **cpp-httplib** (single-header, MIT). Control changes are tiny GET/
  POST hits (`/ctl?id=amp0&v=0.5`); meters polled via `GET /meters` (upgrade to
  SSE/WebSocket later if wanted). Localhost round-trip is sub-millisecond, so an
  envelope-trigger button feels instant.
- Shared control state between the HTTP thread and the audio thread via
  lock-free `std::atomic<float>` per control (no locks in the audio callback).
- UI: `panel.png` as the background, controls absolutely positioned over the
  real panel locations; extra host controls (master volume, per-CV input
  sources, module picker) in side strips; a bottom **status bar** shows the
  value of whatever control is hovered.
- Port: **localhost:4343**. One `emulator/run.sh` builds (CMake) + launches;
  runnable from CLion too. Firmware `make` build is untouched.

Ports of note: everything emulator lives under `emulator/`; the only shared
firmware code is the hardware-free DSP header (`foxtail_dsp.h`, repo root), which
both the emulator and (later) `FoxTail.cpp` include.

### 3.2 Note on units (voltages vs normalized floats)

Confirmed against `time_machine_hardware.cpp`: all volt→float scaling happens in
the board-support layer. Above it everything is normalized — knobs/sliders
`0..1` (`GetTimeKnob`, `GetLevelSlider`), CV inputs `−1..+1` (`InitBipolarCv`,
`GetLevelCV`). The `Controls` seam therefore carries normalized floats, and the
emulator's CV sources (osc/env) output in that same range. No volts anywhere in
Fox Tail's DSP. Emulating a source as "a ±5 V LFO" would just mean replicating
the board's volt→±1 scaling — a later realism refinement, not needed for v1.

Decoupling the *original Tap-O-Matic* through the same seam is feasible (the
delay DSP in `dsp.h` is already hardware-free); the non-trivial parts are the
QSPI-flash calibration, the 150 s SDRAM buffer, and the calibration routine —
all stubbable but woven into working firmware, so out of scope (regression risk).
