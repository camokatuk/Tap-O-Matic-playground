# Fox Tail — open work

Picked up from the session that fixed the crossed output channels. Nothing here
is blocking; the firmware is in a good state.

## 1. Rework what the sliders and pots do

The complaint: turning a band slider up introduces more partials *at once*, and
panning whole bands sounds crude. The first two bands cover the low/low-mid and
don't really need independent pan.

Proposed map (to be tried in the emulator first):

- **Pots 3..9** — stop being pan. Add harmonics *within* the band gradually,
  via the soft-step already used for Cluster's partials-per-cluster. Turning a
  band up should feel like filling it in, not switching a block on.
- **Pots 1..2** — become a stereo spread pair for the whole bank. Pot 2 = spread
  amount, pot 1 = fades between spread *shapes*.

Spread shapes worth trying (undecided — try several):

- Even partials one way, odd the other. **Must interact with switch 2** (ODD
  ONLY): with the even partials already gone, an even/odd spread collapses to
  mono, so that mode needs its own behaviour.
- A curve on how far partial *k* is thrown as a function of *k*: highs always
  fan fully L/R, lows either stay centred (at 0) or fan out too (at max).
- Pan the first *n* partials in pairs — 2L/2R, then 3L/3R, etc.

Any of these is a **control-map** change: it lives in `foxtail_dsp.h` and
`FoxTail.cpp` and ships to every unit. Do it in the emulator first, where the
shapes can be compared quickly.

Note the panel legend still shows delay-era labels, so relabelling waits until
this map settles.

## 2. Review the `kF0Max = 6000` ceiling

`foxtail_dsp.h`. The stated reason is to stop a railed CV (+5 V = +5 octaves)
from pushing every partial past Nyquist and silencing the module.

But the render loop already fades each partial individually as it approaches
Nyquist (`nyqFade`, `foxtail_dsp.h:486`), so the low partials survive on their
own. The ceiling may be doing work that per-partial fade already does. Goal:
let the fundamental track the full audio range and just let the ultra-high
partials fall away, instead of clamping the root.

Check before changing: whether anything else assumes an audio-rate `f0`, and
whether a railed CV really does go silent with the clamp lifted.

## 3. Why is `kF0Min = 8 Hz`?

No recorded reason — the comment next to it only justifies the ceiling. Nothing
found that would break at LFO rates: the phase increment is
`(uint32_t)(incHz * 2^32 / sr)`, which at 0.01 Hz is still ~894, far from
truncating to zero, and band placement uses the *ratio* to `f0` rather than
absolute frequency, so it is unaffected.

That is a code reading, not a test. Worth actually trying `kF0Min = 0.01f` and
listening: a 96-partial bank at LFO rates is potentially a good drone/texture
mode, but nothing has been verified by ear.

## 0. Cluster gain: dynamic compensation failed twice — next is a knob LUT

Goal (still valid): keep high-density Cluster from slamming the clip without
robbing the normal modes of level.

**Post-mortem of the "8k harmonics" bug** (hardware-only artifacts, Cluster
mode, tones as loud as the signal itself, tracking whichever slider is up).
Two implementations of dynamic gain-riding produced it and were rolled back:
the stashed peak-predicting limiter and a power-matching compensation (both
rode `master` with a block-rate gain derived from the engine's per-partial amp
state). Dead hypotheses — do NOT re-test:

- ~~ADC noise into Cluster's floor()~~ — backlash conditioning was built for
  this and the bug survived it; `tests/noise_spur.cpp` injects ADC-scale noise
  offline and shows nothing.
- ~~Knob conditioning~~ — was its own separate bug (audible clicks: dense LSB
  noise → sparse larger steps). Removed; raw knobs are clean with the
  taper-outside fix. Do not bring it back.
- ~~CPU starvation~~ — slimmed comp measured 79.8 avg / 82.9 max (same load as
  the artifact-free build) and the overtones were unchanged and loud.

What was never explained: how a smoothed block-rate gain on `master` produces
full-level ~8 kHz tones on hardware while the emulator and an offline
noise-injected sim stay clean. Whatever it is, it follows the amp-state →
master feedback structure. Both attempts shared that; the next one must not.

**Next approach (user's): a static lookup.** Gain as a function of the Cluster
knob positions only (density, partials-per-cluster, window, position — all
already smoothed control values), precomputed offline from `tests/clip_sweep.cpp`
measurements into a small table baked into the firmware. No dependence on any
runtime engine state. Interpolate the table, fold into `master` or `kHeadroom`.
To build it: extend clip_sweep to dump RMS over a (B, A, win, pos) grid and
fit/tabulate the inverse.

## 4. Cluster mode vs Pigments: partial allocation, not a window

User observation (to discuss before touching code):

Pigments' harmonic engine has no window-size control. It has a number of
clusters and a number of partials per cluster, and the two trade off against a
fixed partial budget: turning both up *takes partials away from the other
clusters*, so at the extreme the whole sound scoots down onto the fundamental.

Our implementation instead shifts partials inside a fixed grid: with the window
wide and both knobs up there are still two or more cluster fundamentals spread
evenly across the spectrum until the very end of the travel. Consequence: the
harsh high tail of the cluster sound is hard to get rid of or reuse — its
energy stays parked up top instead of being reallocated downward.

Possible direction: make density/size reallocate the bank toward the low
clusters (Pigments-style budget) rather than only collapsing within clusters.
Interacts with the spectral tilt (amplitude follows the *shifted* frequency).
Reference: `docs/claude/pigments-harmonic-engine.md`. Emulator first.