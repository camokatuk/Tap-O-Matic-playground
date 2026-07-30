# Fox Tail — repo architecture

How the Fox Tail firmware coexists with the Tap-O-Matic delay and the desktop
emulator. The *instrument* design (control map, engine, measured CPU/memory
facts) lives in `control-maps.md`; the noise constraints in `whinebug.md`.

Naming: the delay firmware stays **Tap-O-Matic**; the oscillator firmware is
**Fox Tail**. Shared board support stays `time_machine_hardware.*`.

## Firmware selection — Makefile module switch

One application .cpp per firmware, each with its own `main()`; the Makefile
picks one:

```
make                  -> Tap-O-Matic delay   (TimeMachine.cpp, -Os, stock)
make MODULE=foxtail   -> Fox Tail oscillator (FoxTail.cpp, -O3 -fno-math-errno)
```

The delay's source and flags are never touched — whinebug concluded it is
already at its optimum. The per-module flags are load-bearing for foxtail (see
control-maps.md "Hard-won rules").

## Three layers — keep sources out of firmware

1. **`foxtail_dsp.h`** (repo root): the entire synthesis engine, hardware-free,
   **C++14-safe** (firmware is `-std=gnu++14`; no `std::clamp`, no
   `std::optional`). The `Controls` struct is the seam. Included by both the
   firmware and the emulator, so what you hear on the desktop is what the
   module plays — and the emulator also processes audio in the firmware's
   5-frame blocks so control/CV rates match (9.6 kHz). Never weaken this
   equivalence; it is the emulator's entire value.
2. **`FoxTail.cpp`** (repo root): thin hardware shell — reads
   `time_machine_hardware.*` into `Controls`, V/oct calibration
   (QSPI-persisted via `PersistentStorage`), LED views, serial diagnostics.
   No signal generators here.
3. **`emulator/`**: native host (miniaudio → CoreAudio + cpp-httplib; run
   `./emulator/run.sh`, UI at http://localhost:4343). `src/sources.h` holds
   emulator-only stand-ins for patched modules (envelope/LFO CV sources); the
   firmware never includes it. Web UI is small thematic files sharing an `FT`
   namespace: `app.js` (glue, panel controls, persistence via localStorage),
   `sources.js` (CV source cards; ranges live in its `RANGES` object),
   `labels.js` (panel-label editor — writes the SVG, re-renders the PNG),
   `partials.js` (partial viewer fed by `/partials`, which publishes the
   engine's own per-partial state — the viewer computes nothing itself).

## Units

Everything normalized at the seam, matching what the board support hands the
delay: knobs/sliders 0..1, CVs −1..+1, pitch CV in octaves (`2^cv`). No volts
inside the DSP.