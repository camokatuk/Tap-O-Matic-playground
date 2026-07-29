# Reference — Arturia Pigments 7, Harmonic engine

Extracted from `pigments_Manual_7_0_0_EN.pdf` (chapter 10, printed pp. 122–131;
PDF page indices 127–136), plus the Modal engine's Warp/Shaper (pp. 135–136) which
covers similar ground. Condensed, not verbatim. Kept because it's the closest
commercial reference to what Fox Tail is trying to be.

## Scale

**512 partials max** — same ceiling we were arguing about. A "Partials Limit"
dropdown caps the count below that to save CPU, and the manual explicitly says
8 or 16 partials is a good place to learn. So 512 is a soft-synth ceiling, not a
floor, and small counts are considered musically legitimate.

## Partials section

- **Partials knob** — number of partials, brought in "in progressively decreasing
  volume by default". So there's a built-in rolloff (≈1/n); the raw series is
  saw-like, not flat.
- **Partials Volume** — overall level of the partial bank. *"Fractional values
  decrease the volume of the highest-pitched partial in the series"* — i.e. the
  fractional part of the count fades the topmost partial in. This is exactly the
  soft-step / apodized-truncation idea, shipped commercially.
- **Partials Limit** — dropdown ceiling on the count, purely a CPU control.
- **Random Phase** button — "randomizes the phase of the partials, which can
  enrich or thicken the sound". Confirms the crest-factor trick is a user-facing
  feature, not just an implementation detail.
- **Smooth** button — partials changing amplitude due to modulation do so more
  gradually. Our per-partial gain smoothing, exposed as a toggle.
- **Partials viewer** — horizontal axis pitch, vertical axis volume; vertical
  position *also* encodes stereo pan (above centre = left, below = right).

## Frequency / Phase Mod section

FM or PM applied to the whole partial series, sourced from the engine's Modulator
oscillator. **Ratio** knob (−1.00 to 5.00) sets the modulator's ratio to the
fundamental; **Amount** sets intensity.

## Shape section (spectral profiles)

Superimposes a "frequency profile" on the raw series — functionally a multi-point
EQ curve that notches out multiple frequencies.

- Two **Spectrum** slots A and B, 12 shapes each.
- **Morph** — continuous crossfade A↔B. Modulating it gives vowel-like
  "ee-ah-ow" motion.
- **Section** — shifts the spectrum's position over the partial series (changes
  which partials it affects).
- **Depth** — how strongly the spectrum applies.
- **Tilt** / **Tilt Offset** — slope steepness, and the partial where the slope
  starts.
- **High-pass / low-pass** icons — roll off below / above the spectrum's range.
- **Parity** — continuous odd/even balance of the multiples. All odd, all even,
  or anything between. Cheap and very effective.

## Imaging section (stereo)

Pans different partials across the stereo field. Three modes:

- **Split** — manual: *Odd* pans odd partials L/R, *Even* pans even partials L/R.
- **Random** — randomly pans individual partials. *Rate*, *Depth*.
- **Periodic** — pans *clusters* of partials. *Periods* (cluster size), *Depth*.

## Partial Shaper section — the interesting one

One section, three modes, **four knobs each**. All three share Position + a size
parameter; only knobs 3 and 4 change meaning between modes.

| | knob 1 | knob 2 | knob 3 | knob 4 |
|---|---|---|---|---|
| **Window** | Position — where the window starts (lowest partial) | Win Size — how high it extends | FM — applies modulator FM to partials *inside the window only* | Gain — level of partials inside the window only |
| **Cluster** | Position — lowest partial of the starting cluster | Clusters — window width, which sets how many clusters there are | Partials — partials per cluster | Density — how far partials shift *toward the start of their cluster* |
| **Shepard** | Position — base partial of the window | Win Size — window width | Phi — amount of frequency shift toward the next partial up | Gain — level inside the window |

- **Cluster** brings partials within a window closer together, changing their
  frequencies and the harmonic relationships. Manual's tip: Density at or near
  25%, 50%, 100% gives the most conventionally musical results.
- **Shepard** shifts each partial's frequency toward the next higher partial.
  Manual's tip for the actual illusion: **modulate Phi with a slow ramp LFO**,
  Phi at 0.500, mod depth 0.50. (In Eurorack that's just a ramp LFO patched to a
  CV jack.)

## Modal engine, Warp + Shaper (adjacent, worth stealing from)

- **Warp** — expands/compresses the whole partial distribution relative to the
  fundamental; bipolar. Gives inharmonic / metallic / ring-mod-like results.
  - *Range* — the partial below which nothing is warped.
  - *Shape* — subtly alters the warp curve.
  - *Q* — quantizes warped partials back onto the harmonic series ("your best
    friend for keeping things pleasant").
- **Shaper** — morphs between pairs of "masks", overlays where some harmonics are
  fully muted and others pass. Nine mask pairs, one Morph knob.
- **Stereo Spread** — *Spread* pans alternating harmonics; *Detune* detunes the
  left- and right-panned harmonics against each other (odd partials up, even
  down). Small amounts = chorus, large = woozy.

## Takeaways for Fox Tail

1. 512 is the right ceiling to design against, and small counts stay useful.
2. Built-in ≈1/n rolloff, random phase, and amplitude smoothing are all standard
   — matches what `control-maps.md` already concluded independently.
3. The fractional part of a count control fading the top partial is a shipped,
   proven behaviour (the "soft-step" question).
4. The Partial Shaper's shape — one **mode switch + four knobs**, with two of the
   four consistent across modes — is a direct fit for our 4 free knobs + switches.
5. Panning individual partials/clusters (Imaging) is a big part of why this engine
   sounds impressive, and our 9 pots are silkscreened **PAN**.