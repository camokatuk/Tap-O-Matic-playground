# Fox Tail — open work

Nothing here is blocking; the firmware is in a good state. The control map is
settled and hardware-verified — `control-maps.md` is current. Remaining before
the panel labels are cut: nothing, unless item 1 changes GATE's job again.

## 1. GATE input, and sample & hold off the audio inputs

Idea: sample something into an internal modulator off the audio inputs —
internal modulation without spending a knob. NOTE: GATE now carries parity, so
this needs a different trigger, or parity and the S&H share the jack somehow.

**Blocked on a question first.** `CLAUDE.md` lists "never read the audio input"
as part of the whinebug noise budget, on the theory that reading it contributes
to the noise. That theory has not been checked. Read `whinebug.md` and find out
whether the rule is about the ADC, the codec, or something else before designing
anything on top of it. CPU is not the concern — a load per frame is nothing.

## 2. Review the `kF0Max = 6000` ceiling

`foxtail_dsp.h`. The stated reason is to stop a railed CV (+5 V = +5 octaves)
from pushing every partial past Nyquist and silencing the module.

But the render loop already fades each partial individually as it approaches
Nyquist (`nyqFade`), so the low partials survive on their own. The ceiling may
be doing work that per-partial fade already does. Goal: let the fundamental
track the full audio range and just let the ultra-high partials fall away,
instead of clamping the root.

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

## 4. OPTIONAL — Shepard's pan roll moves the static partials too

A wrap advances `phiRot_`, which rotates the whole `panPartial_` table, so every
partial takes its neighbour's pan slot — including the ones outside the window,
which never moved. Measured on a static partial (ratio fixed at 3.000, spread
0.15): its pan cycles through all 8 slots, one step per wrap, repeating every 8.

For partials INSIDE the window the roll is load-bearing — remove it and the same
patch goes from 5.7x the median sample step to 17.5x, because a frequency
changing hands needs its image to follow. Only the static ones are wrong.

**Deliberately left in: it sounds good.** Image movement that used to require pot
3 past the ORBIT knot (`kPanRotAnchor`, 0.5+) now starts as soon as the slots are
non-coincident, driven by the wrap instead of by the pot.

Fixing it needs a per-partial choice between a rolled and an unrolled pan table
in the render loop — roughly two instructions in the Shepard clone, which is the
size of change `archives.md` records as bringing hardware artifacts back with the
audio bit-identical. So: hardware A/B, not a free edit.

## 5. Cluster's gain compensation scales partials the window never moved

`norm` in `UpdateBlock` is folded into the per-band gains, so it reaches **every**
partial — including the ones outside the shaper window, which never collapsed and
have nothing to compensate for.

Measured (old engine, window from partial 5, 9 wide, max cluster): partial 1 sits
outside the window and still loses **4.4 dB** when the density knob sweeps
0 → 1. It has always erred quiet, which is why no guard caught it.

It stops being harmless with the collapse-UP direction, where the compensation
*boosts*: a wide window clearing the fundamental boosted that static fundamental
by the collapse's full 5.5x, one partial reaching peak 1.17 against the 0.85
knee. Only reachable today with the one-partial window shoulder, which is why
that shoulder is gated to Shepard — the gate is a workaround for this, not a
design preference.

The fix that was tried and reverted: weight the boost by the share of the bank's
POWER inside the window, `boostW = 1 + winPow*(boost - 1)`, with `winPow` from
integrals of `r^-2a` over the window, morphed with the tilt like the boosts
themselves. It is exact at `winPow = 1` (window wide open — the case the
compensation was built for, so the headline patches are untouched), and it cut
that 4.4 dB error to 0.9 dB. It was reverted because it makes Cluster **louder**
whenever the window is partial, and Cluster's levels were tuned by ear against
the current behaviour.

Before landing it: run the clip sweep, and A/B a partial-window Cluster patch on
hardware. The model holds total POWER flat; the guard measures PEAK, and those
part company exactly when a cluster piles onto one frequency.

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

Mechanism: a collapsed cluster's power is ~ln(1+t)/t under the 1/sqrt(r) tilt
but ~1/(1+t) under 1/r, so each slope has its own compressed:spread ratio and
the two blend with the tilt morph; `norm = 1/sqrt` of that holds RMS flat across
the density knob. `Log1pOverX` is libm-free — the first version called `logf`
per block and that burst was audible on hardware.

**The closed form is tied to the tilt exponent.** Changing the tilt without
re-deriving it left the compensation solving the old spectrum and the density
knob swung 6.4 dB; it is now back to 0.9. If the tilt law changes again, redo the
integral — nothing else catches it.

Peak was the open worry here and it has now been measured: worst peak 0.497 over
37,050 patches against the 0.85 knee, and the bright tilt is the tamer of the
two. No headroom gap outstanding. Kept only as a pointer to the derivation.
