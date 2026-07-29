# Fox Tail — control map (living doc)

Mapping the panel onto a large partial bank. Companion to `oscillator-impl.md`
(architecture), `whinebug.md` (noise budget), and `pigments-harmonic-engine.md`
(the commercial reference this borrows from).

## The panel we have to spend

| Control | Count | Physical | CV? |
|---|---|---|---|
| Level sliders | 9 | big, gestural, in a row | yes, 1 each |
| Pan pots | 9 | small, one under each slider | no |
| TIME / SPREAD / FEEDBACK / HPF / LPF | 5 | knobs | yes, analog, 1 each |
| Switches (`fbMode`, `filterPos`) | 2 | 2-position | — |
| GATE | 1 | jack | digital only |
| LEDs | 9 | one per slider | output |

## The map

**Sliders = gain, pots = pan.** The pots are silkscreened PAN, panning individual
partials is a headline feature of the Pigments engine, pan stays meaningful even for
a one-partial band (unlike any within-band parameter), and today's `foxtail_dsp.h`
throws the stereo output away by writing the same sample to L and R.

Brightness / partial count is **not** a knob — it's the top sliders. Pulling band 9
down *is* the lowpass, with 9 bands of resolution and a CV jack per band.

### How the 9 sliders address the bank

**Octaves, always.** Constant-Q bands 1, 2–3, 4–7 … 256–511, tiling 512 exactly.
The slider row literally draws the spectrum, and constant-Q ≈ how hearing bands
things. Implemented as breakpoints of a piecewise-linear envelope in
log-frequency rather than 9 rectangular blocks — the crossfade between adjacent
bands is what keeps a partial's gain continuous as Cluster or Shepard glides it
across a band edge.

A `1/√r` tilt does the work of both the spectral slope and the per-band
normalisation at once: band *b* holds 2^b partials at amplitude 2^(−b/2), so every
octave carries equal power and all sliders up gives a pink-ish spectrum instead of
something top-heavy.

*Considered and dropped:* **Swarm** (slider = one harmonic, expanded to ~56
detuned partials). With all four knobs committed to the shaper it had nowhere to
get a detune-spread control, and without one it collapses to a single loud partial
per harmonic. Cluster just below density 1 produces the same beating/chorus anyway.
Dropping it freed switch A for random-phase.

### Switch A — partial phase, aligned vs random

### Switch B — Cluster vs Shepard

Both are per-partial frequency shifts applied **within a movable window**, ~1 FMA per
partial per block. The window is the point, not overhead: partials outside it stay
anchored and hold the pitch stable while the ones inside go weird. Position and size
are both CV-able, so the window itself is playable.

| Knob | Cluster | Shepard |
|---|---|---|
| TIME | pitch | pitch |
| SPREAD | Position — partial where clustering starts | Position — base partial of window |
| FEEDBACK | Clusters — window width, sets how many clusters | Win Size |
| HPF | Partials per cluster (**soft-step**) | Phi — shift toward the next partial up |
| LPF | Density — how far partials shift toward their cluster start | Gain inside the window |

Only knobs 3–4 change meaning between modes; Position and Size are constant. That's
what makes a screenless panel survive a mode switch. All four labels end up lying —
the SVG label editor exists, relabel once the map settles.

- **Cluster** — `r_k = k·(1−density) + density·s_c`, s_c = start of k's cluster.
  Density 1 collapses each cluster onto one frequency. Near 25/50/100% is the most
  conventionally musical (Arturia's tip).
- **Shepard** — `r_k = k + phi` inside the window. Phi + a ramp LFO into its CV jack
  = a real Shepard tone, as a patch rather than a feature.
  The illusion works because the envelope is sampled at the **shifted** frequency,
  not at the partial index: at phi=1 partial *k* occupies the frequency partial
  *k+1* had at phi=0, so the ensemble is identical across the wrap and nothing
  clicks. Measured flat to 0.17% across a full phi sweep. Practically: open the
  window wide and shape the sliders into a hump and you get the illusion — **you
  draw the Shepard envelope with the sliders**. Narrow the window instead and it's
  a shear on a slice, with everything outside anchored.

Dropped from Pigments on purpose: **Window** and **Warp** modes (more mode-dependent
knobs than the panel can carry), and the **Shape** section (2 spectrum slots × 12
presets + morph/tilt) — nine sliders drawing the spectrum by hand beat 12 presets.
**Parity** (continuous odd/even balance) is the one cheap omission worth a home.
GATE (digital) is unassigned; could re-randomise phases.

## Engine

**One shared sine LUT, N independent phase accumulators** — *not* IFFT-to-wavetable
(Odessa-style), which retunes only by rebuilding the table and is bursty by
construction. A magic-circle resonator is cheaper per sample but needs a sin/cos pair
per partial per retune; Cluster and Shepard retune continuously (512 × 9600 blocks/s
≈ 4.9 M transcendentals/s — dead on arrival). LUT + fixed-point phase retunes with one
multiply. **LUT wins.**

- **Precomputed at init:** sine table (2048 × f32 = 8 KB), static ratio/band tables.
- **Per block (9600 Hz):** `inc[k] = r_k·f0/sr`, `amp[k]`. 512 × 9600 = 4.9 M
  partial-updates/s — the shaper's real cost, and constant work.
- **Per sample:** phase accumulate (uint32, free wrap), table lookup + lerp, MAC.
- State: 512 × 16 B = 8 KB.

Budget at 480 MHz:

| | inner loop (~12 cyc) | per-block control (~15 cyc) | total |
|---|---|---|---|
| 512 partials | 61% | 15% | **~76%** — at the edge |
| 256 partials | 31% | 8% | **~39%** — comfortable |

Cycle counts are optimistic (M7 is in-order; load-use hazards). Partial count is one
`constexpr` in the shared header — measure on hardware, ship what fits.

**No sub-rate tricks.** Halving the control-update rate, or round-robining partial
updates across blocks, would buy ~7% — and both are banned by whinebug rule 2 ("no
periodic structure slower than the callback rate; any sub-rate pattern becomes an
audible comb at the pattern rate"). Full update every block or nothing. Partial count
is therefore the *only* headroom lever.

Dropping shaper *modes* saves flash, not cycles; only dropping retuning altogether
would recover the 15%.

### Memory (verified against libDaisy's linker scripts)

| Region | Size | Notes |
|---|---|---|
| DTCMRAM `0x20000000` | 128 K | zero wait state, core-coupled, **not cached**, no bus arbitration |
| AXI SRAM `0x24000000` | 512 K | **where `.data`/`.bss` land by default**; behind cache + AXI |
| RAM_D2 `0x30000000` | 288 K | DMA buffers (`.sram1_bss`), where audio DMA lives |
| ITCMRAM `0x00000000` | 64 K | region declared but **no output section maps to it** |
| internal FLASH | 128 K | `.text` runs here, covered by I-cache |
| SDRAM `0xC0000000` | 64 M | external — banned by whinebug rule 1 |
| QSPI | 8 M | external serial |

**Does a table in memory reintroduce whine? Not if it's in DTCM.** whinebug's finding
is that the whine scales with *memory traffic per second* — but that was external
SDRAM traffic: an off-chip FMC bus toggling wide address/data lines, plus refresh
cycles, plus an external chip's own current bursts. DTCM is on-die, zero wait state,
uncached, no arbitration: every sample costs identically, which is exactly the
constant-current behaviour rule 2 asks for. It's the quiet choice, and 8 KB of table +
8 KB of state in a 128 KB DTCM is nothing.

The trap is the **default**: `.data`/`.bss` go to AXI SRAM, which is cached — so access
time varies with hit/miss and refills burst over AXI. Use libDaisy's
`DTCM_MEM_SECTION` (`daisy_core.h:31` → `.dtcmram_bss`) for the table and all partial
state. Code can stay in flash; the inner loop is small enough to sit in I-cache after
the first pass. Moving it to ITCM would need a custom output section added to the
linker script — a later optimisation, not a prerequisite.

**Caveat that must be measured, not reasoned about:** whinebug's "a lightweight
additive oscillator will be OG-quiet on this board" was proven by a *near-idle*
measurement. A 512-partial engine at ~76% CPU is not near-idle. The traffic is on-die
and steady, which is the best case, but the average current draw is far higher than
what was measured. If 512 measures worse than 256 on the bench, that's the answer.

**Nyquist.** Harmonic 512 clears 24 kHz only below f0 = 47 Hz.

| f0 | 27.5 | 55 | 110 | 220 | 440 | 880 |
|---|---|---|---|---|---|---|
| harmonics available | 872 | 436 | 218 | 109 | 54 | 27 |

Partials above Nyquist must fade out, not switch off. Bands going dark as you play up
is honest, and the LEDs can show it.

## Soft-step ("semi-discrete") controls

Discrete steps that keep doing something in between. No settled name; nearest are
**fractional count** (integer part + frac gating the next element's gain — same math as
fractional delay) and **apodization**. Pigments ships exactly this: the fractional part
of its Partials knob fades the topmost partial in.

Only worth it at small counts — fading partial 300 of 512 is inaudible. Used here for
partials-per-cluster.

## Gain staging

Partials at different frequencies are incoherent → loudness follows power (Σa²), peak
follows amplitude (Σa). They diverge by √N; that gap is the problem.

1. **Phase dispersion — free, do it first.** All partials at phase 0 realign every
   period → peak = Σa (an impulse train, worst possible crest factor). Random phases
   drop peak from N·RMS to ≈√(2 ln N)·RMS. Phase is inaudible for a static spectrum
   (Ohm's law of acoustics) → free headroom. Pigments ships this as a button, and it's
   the eventual job of switch A.
2. **1/k tilt baked into the series** — Σ converges, all-sliders-up = saw (useful home
   position). Pigments' partials come in "in progressively decreasing volume" too.
3. **Power normalisation** g = 1/√(Σa²) keeps loudness constant, but auto-gain makes one
   slider duck the others — feels broken to play. **Peak normalisation** g = 1/Σa never
   clips but audibly quietens as partials are added. Neither is used.

**Chosen:** fixed headroom budget, no dynamic normalisation. Per-band 1/√M weighting +
1/k tilt, sized so everything-at-full ≈ −12 dBFS RMS, random initial phases, gentle
tanh backstop. Sliders behave as the panel promises: more up = louder, graceful
saturation at the top. This is the Hammond drawbar answer, which isn't a coincidence —
a 9-slider additive panel *is* a drawbar organ.

## Status

**Implemented and bench-verified in the emulator; untested on hardware.**
`foxtail_dsp.h` is the shared LUT engine; `FoxTail.cpp` and the emulator both run
it. Bench results (native harness):

| check | result |
|---|---|
| Cluster ratios, M=2 | `1, 1.5, 3, 3.5, 5, 5.5` at d=0.5; pairs onto odd harmonics at d=1 |
| Shepard seamlessness | level flat to **0.17%** across a full phi sweep, phi=0 == phi=1 |
| Crest factor | **3.18** random phase vs **6.81** aligned (aligned also clips) |
| Level, all sliders up | −15.8 dBFS RMS, peak 0.51 |
| Shaper sweep, 810 settings | all finite, worst peak 0.87 |
| Object size | 16.1 KB — fits DTCM with room to spare |

Open on hardware: the CPU budget, the switch polarity in `FoxTail.cpp` (pull-ups,
guessed), and whether 76% load changes the noise picture.