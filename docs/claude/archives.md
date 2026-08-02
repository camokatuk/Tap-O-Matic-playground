# Archive — closed investigations

**You do not need this file to work on the firmware.** Read it only when you
want the history behind a decision: a bug that was already diagnosed, an
approach that was already tried and rolled back, a measurement already made.
Nothing here describes current behaviour, and nothing here is an open question.

Every story below left a one-line rule in a live doc (`CLAUDE.md`,
`control-maps.md`, `whinebug.md`, `todo.md`). The rule is what matters; this is
the evidence behind it, kept so nobody re-runs the experiment.

---

## The whine — full attribution

Closed. The live summary and the design rules that came out of it are in
`whinebug.md`.

Measured at block size 5, output boosted +35 dB, sliders down. Isolated by A/B
firmware tests: mute the delay DSP, stop the control ADC scanning, or both, and
compare spectra.

| Spectrum component | Source | Fixable in software? |
|---|---|---|
| Tone forest below 9.6k (~1.7, 2.4, 4.8, 7.2, 7.9 kHz) + white-noise floor elevated ~7 dB vs OG | Delay engine DSP + external-RAM streaming | Only by removing the workload (an oscillator firmware has none of this) |
| 9.6 kHz + 19.2 kHz | Per-callback control-scan burst (~50 muxed inputs read + processed in one thump per block) | Yes, but not worth it — see the round-robin section below |
| ~12 kHz + quiet ~10 kHz | Hardware floor. The stock OG Time Machine shows the same signature | No |

With DSP muted **and** ADC scanning stopped (chip near idle), the Tap-O-Matic
measures as quiet as the OG. That is the floor, and it is reachable.

Ruled out: LEDs (`setLeds = false` changed nothing), raw clock frequency.

### The delay amplifies its own whine

The whine couples into the codec **input** stage, gets recorded into the delay
buffer, and is re-mixed by the taps. Evidence: on the OG, individual sliders can
make its 12k tone quieter (phase cancellation between delayed copies of a
coherent sine); at minimum time all taps sum in phase and the whine jumps out.

### Why the OG is quieter: it's the board, not the code

The OG runs the same firmware lineage — same 150 s buffer, 9 taps, external-RAM
streaming, similar control scanning — yet shows only the hardware-floor tones
(no spur at its ~6.9 kHz callback rate). Meanwhile a near-idle Tap-O-Matic
matches the OG's spectrum. Conclusion: both boards generate the same digital
noise; **the DIY carrier board couples far more of it into the audio path**
(power filtering / ground routing / layout). Software can shrink the noise
source but cannot fix the leak. Making the *delay* OG-quiet would take a
hardware revision (KiCad files are in the repo; suspects: supply filtering,
decoupling, ground plane).

Shrinking the buffer (150 s → 30/60 s) would NOT help — the whine scales with
memory traffic per second, which is independent of buffer length.

---

## Round-robin control scanning — tested and rejected

Rule it produced: *do not re-attempt round-robin or any sub-rate control scan.*

Spreading the control scan across callbacks (one handler group per block instead
of one big burst, slew-compensated) was implemented and measured:

- A fixed scan order repeats at callbackRate/14 ≈ 686 Hz and produced a comb of
  its harmonics (prominent 6.8 kHz spur = 10th harmonic). The stock 12k peak
  partly disappeared — it was partly an intermod product (9.6k + 2.4k).
- Shuffling the order each cycle (spread-spectrum style) did not remove the
  comb: the rigid 14-block cycle still concentrates energy at frame-rate
  harmonics.
- **Subjectively worse than stock.** Ear sensitivity peaks at 2–5 kHz and falls
  steeply above ~10 kHz. Stock parks its spurs at 9.6/12/19 kHz — the least
  audible placement available. Any scan-spreading relocates energy downward into
  more audible territory.

Rolled back. The block size of 5 inherited from Eris's tweaks is accidentally
near-optimal psychoacoustics for this firmware.

---

## Knob conditioning — tested and rejected

Rule it produced: *knobs 2–5 and their CVs are read raw; do not add
conditioning.*

A backlash-gate + one-pole conditioner was built to stop Cluster's `floor()`
turning ADC LSB noise into per-block frequency hops. It audibly clicked: it
converts dense tiny noise into sparse larger steps, which is worse. With the
window taper moved outside the window, raw knob noise is inaudible (hardware
A/B, 2026-08).

---

## Cluster gain compensation — the "8k harmonics" post-mortem

Rule it produced: *no gain that feeds engine amp state back into `master`.* The
open replacement plan (a static knob-position LUT) is in `todo.md`.

The bug: hardware-only artifacts in Cluster mode — tones around 8 kHz as loud as
the signal itself, tracking whichever slider was up. Two implementations of
dynamic gain-riding produced it and were rolled back: a stashed peak-predicting
limiter, and a power-matching compensation. Both rode `master` with a
block-rate gain derived from the engine's per-partial amp state.

Dead hypotheses — do NOT re-test:

- ~~ADC noise into Cluster's `floor()`~~ — backlash conditioning was built for
  this and the bug survived it; `tests/noise_spur.cpp` injects ADC-scale noise
  offline and shows nothing.
- ~~Knob conditioning~~ — that was its own separate bug (see above). Removed;
  raw knobs are clean with the taper-outside fix.
- ~~CPU starvation~~ — the slimmed compensation measured 79.8 avg / 82.9 max
  (same load as the artifact-free build) and the overtones were unchanged and
  loud.

Never explained: how a smoothed block-rate gain on `master` produces full-level
~8 kHz tones on hardware while the emulator and an offline noise-injected sim
stay clean. Whatever the mechanism, it follows the amp-state → master feedback
structure. Both attempts shared it; the next one must not.

---

## Gain staging — schemes rejected

Rule it produced: *fixed headroom budget, no dynamic normalisation* (the
Hammond drawbar answer). Live details in `control-maps.md`.

- **Power normalisation** — raising one slider ducks the others; feels broken.
- **Peak normalisation** — the patch audibly quietens as partials are added.

---

## Output clip — why it is not a cubic

Rule it produced: *linear below a 0.85 knee, parabolic to a hard cap at 1.15.*

The original clip was a cubic with no linear region, so every patch wore ~−35 dB
of always-on waveshaping. Below the knee the output stage is now bit-exact.
`tests/run.sh` (grid + 1500 random patches at the firmware block size): 42 of
4380 patches engage the clip, all Cluster at high density, worst −5.5 dB
residual; every non-Cluster patch is untouched.

---

## Features considered and dropped

None of these are open proposals; they were each weighed against the panel's
fixed control count and lost.

- **Swarm mode** — a slider expands one harmonic into a detuned cluster. No knob
  left for its spread, and Cluster near density 1 gives the same beating.
- **Pigments' Window/Warp shaper modes** — more mode-dependent knobs than the
  panel carries.
- **Pigments' Shape presets** — hand-drawn sliders beat 12 presets.
- **A master-level pot** — a fixed internal level does the job.
- **An inharmonicity-onset pot** (partial below which the series stays harmonic,
  cf. Pigments' Modal Warp "Range") — at high settings only quiet or ultrasonic
  partials bent, so the control read as dead. The k² law already protects the
  low end. That pot became fine tune.