# Fox Tail — open work

Nothing here is blocking; the firmware is in a good state. The slider/pot map is
settled and hardware-verified (see `control-maps.md` once it is brought up to
date — item 1).

## 1. `control-maps.md` is stale

It still documents per-band pan on pots 2..9 and the old 35%-of-a-band gain
crossfade. Neither exists. What replaced them:

- Sliders 2..9 do gain over their whole travel and, above half, **fill** — how
  far the band spreads out from its seed. Bottom half = one partial per band.
- Pots 4..9 place that seed: CCW the band's lowest partial (fills upward),
  centre its middle (fills outward), CW its highest (fills downward). Distance
  is measured in **partials**, so a pot means the same thing in a band holding
  two and one holding thirty-two.
- Pot 2 = stereo spread width; the pan positions are fixed at `kSlots`, assigned
  by `kSlotPat` (invariants in `tests/slot_table.cpp`).
- Pot 3 = shifts the envelope and comb together along the harmonic series,
  ±1 octave, without moving any partial's frequency.
- Cluster samples the envelope at the partial **index**, Shepard at the shifted
  frequency. Sliders pick the ingredients in Cluster and EQ the output in
  Shepard.
- The gain crossfade between bands is **Shepard only** and `kXfadeW` wide.

## 2. Run the clip tests

Deferred deliberately while the map was in flux. `tests/clip_guard.cpp` and
`clip_sweep.cpp` compile against the new `Controls` but have not been run since
the comb, the new spread and the Cluster index change landed. The hard-coded
reference in clip_guard's "density 0 untouched" check moves whenever the pan
layout does — re-measure it with `FOXTAIL_CLUSTER_NORM=0` rather than pasting
the compensated number, or the assertion goes vacuous.

## 3. GATE input, and sample & hold off the audio inputs

Idea: use GATE to sample something into an internal modulator, with the audio
inputs as the sampled source — internal modulation without spending a knob.

**Blocked on a question first.** `CLAUDE.md` lists "never read the audio input"
as part of the whinebug noise budget, on the theory that reading it contributes
to the noise. That theory has not been checked. Read `whinebug.md` and find out
whether the rule is about the ADC, the codec, or something else before designing
anything on top of it. CPU is not the concern — a load per frame is nothing.

## 4. Review the `kF0Max = 6000` ceiling

`foxtail_dsp.h`. The stated reason is to stop a railed CV (+5 V = +5 octaves)
from pushing every partial past Nyquist and silencing the module.

But the render loop already fades each partial individually as it approaches
Nyquist (`nyqFade`), so the low partials survive on their own. The ceiling may
be doing work that per-partial fade already does. Goal: let the fundamental
track the full audio range and just let the ultra-high partials fall away,
instead of clamping the root.

Check before changing: whether anything else assumes an audio-rate `f0`, and
whether a railed CV really does go silent with the clamp lifted.

## 5. Why is `kF0Min = 8 Hz`?

No recorded reason — the comment next to it only justifies the ceiling. Nothing
found that would break at LFO rates: the phase increment is
`(uint32_t)(incHz * 2^32 / sr)`, which at 0.01 Hz is still ~894, far from
truncating to zero, and band placement uses the *ratio* to `f0` rather than
absolute frequency, so it is unaffected.

That is a code reading, not a test. Worth actually trying `kF0Min = 0.01f` and
listening: a 96-partial bank at LFO rates is potentially a good drone/texture
mode, but nothing has been verified by ear.

## 6. Shepard band-boundary tick — known, mitigated, watch it

In Shepard a partial crossing a band boundary stepped in level, which ticked
once per crossing. Mitigated by the Shepard-only crossfade (`kXfadeW`). If it
ever comes back, the repro is: Shepard, slider 3 fully up and slider 4 fully
down, window wide, and sweep knob 4 — partial 4 crosses the boundary at 4.16
when phi ≈ 0.16.

## 0. Cluster gain compensation — SHIPPED, parked as a clipping reminder

A closed-form compensation ships in `UpdateBlock` (`norm`, folded into the
per-band gains, never into `master`). Derived from control values ONLY — the
density/partials/window geometry — so it has none of the amp-state → master
feedback that made two earlier attempts throw hardware ~8 kHz artifacts
(post-mortem in `archives.md`).

Mechanism: a collapsed cluster's power under the 1/sqrt(r) tilt is ~ln(1+t)/t,
so the compressed:spread ratio is R(y)/R(x); `norm = 1/sqrt` of that holds RMS
flat across the density knob. `Log1pOverX` is libm-free — the first version
called `logf` per block and that burst was audible on hardware.

Working well in practice. Kept open only as a **reminder to re-check peak, not
just RMS**: high-density collapse raises crest factor, which is a headroom
question older than the compensation. Re-check as part of item 2.
