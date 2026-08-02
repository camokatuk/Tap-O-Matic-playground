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