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

## 0. Cluster gain compensation — SHIPPED (control-only), parked

Done for now. A closed-form compensation ships in `UpdateBlock` (`norm`, folded
into the per-band L/R gains, never into `master`). It is derived from control
values ONLY — the density/partials/window geometry — so it has none of the
amp-state → master feedback that made the two earlier attempts throw hardware
~8 kHz artifacts (post-mortem in `archives.md`). The knob-LUT idea below was the
previous plan; the closed form replaced it (no offline table to bake).

Mechanism: a collapsed cluster's power under the 1/sqrt(r) tilt is ~ln(1+t)/t,
so the compressed:spread ratio is R(y)/R(x); `norm = 1/sqrt` of that holds RMS
flat across the density knob. `Log1pOverX` is libm-free (a series arm near 0, a
`Log2Fine` polynomial above) — the first version called `logf` per block and
that burst was audible on hardware. Verified on hardware this session: noise
gone, RMS flat within 1.5 dB across the density knob.

**Known-open, not addressed:** `tests/clip_guard.cpp` still FAILS. RMS is held
flat but PEAK is not — high-density collapse raises crest factor (~6.4 vs the
~3.3 `kHeadroom` budgets for), and hard-panning the whole bank into one channel
is +3 dB over the guard's old worst case. Worst measured peak ~0.96 vs the 0.85
knee. This is a **headroom** problem, older than the compensation, exposed by
the improved guard (cfg2 = whole bank one channel; an 8 s window that actually
sees the beat). Options weighed but not chosen: drop `kHeadroom` ~1 dB (honest,
costs level everywhere); change the pan law; push the compensation exponent
toward peak. Left for a later session — deferred with the user.

**Revisit the whole thing if Cluster's behaviour changes** (see item 4): the
compensation geometry assumes collapse-within-clusters. A Pigments-style
partial-budget reallocation would need it re-derived.

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