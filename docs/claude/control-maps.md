# Fox Tail — control map & engine (living doc)

The instrument design: how the panel maps onto the partial bank, and the engine
that renders it. Companions: `oscillator-impl.md` (repo architecture),
`whinebug.md` (noise budget), `pigments-harmonic-engine.md` (the commercial
reference this borrows from), `archives.md` (closed investigations — only if you
need the history behind a rule). Everything below is implemented and
hardware-verified unless marked open.

## The panel

| Control | Function | CV? |
|---|---|---|
| Slider 1 | inharmonicity amount | yes |
| Pot 1 | spectral shift: slides envelope + comb ±1 octave along the series | — |
| Sliders 2–9 | gain of bands 1–8 over full travel; above half, also **fill** | yes, 1 each |
| Pot 2 | fine tune, ±1 semitone (centre = in tune, clockwise = sharp) | — |
| Pot 3 | stereo image: mono → scatter → orbit, then orbit speed/detune | — |
| Pots 4–9 | fill *order* for bands 3–8: CCW highpass, centre bandpass, CW lowpass | — |
| Knob 1 (was TIME) | pitch, 20–2000 Hz log; its jack is calibrated V/oct | yes |
| Knob 2 (was SPREAD) | shaper window start | yes |
| Knob 3 (was FEEDBACK) | shaper window width | yes |
| Knob 4 (was HPF) | **centre-neutral, both modes.** cluster: partials per cluster, CW collapsing onto the cluster's low end and CCW onto its high end / shepard: glissando rate, CW rising | yes, but see below |
| Knob 5 (was LPF) | cluster: density / shepard: window duck (CCW = unity) | yes |
| Switch 1 (left) | down = CLUSTER, up = SHEPARD | — |
| Switch 2 (right) | spectral tilt: up = BRIGHT (1/√r), down = DARK (1/r) | — |
| GATE | parity: high = ODD ONLY, low (unpatched) = ALL partials. Digital only | — |
| LED 1 | inharmonicity amount | — |
| LEDs 2–9 | band levels / shaper-window peek | — |

Hardware facts (measured): switch 1 reads HIGH when down, switch 2 HIGH when up
(opposite polarities). Bands 1–2 hold two partials each, so they have no fill-order
pot; pots 4–9 cover bands 3–8.

**CV nulls.** The summing jacks (knob CVs 2–5, the nine level CVs) do not read
0 unpatched — on this unit they sit at about −0.02, and since firmware adds the
jack to the panel control and clamps, that bias ate the top 2.5% of every
affected control's travel: a knob at full CW reached 0.975, which is why
CLUSTER would not collapse to one cluster at max (density 0.973 leaves ~2.7% of
the original spacing, tens of Hz of beating). The panel controls themselves are
fine — knob 1 is the one big knob with no CV summed into its logged value, and
it hits 0.000 and 1.000 exactly. Each jack's null is captured as step 3 of the
calibration gesture (or alone via a one-shot `CV_NULL=1` build — see CLAUDE.md)
and subtracted in `KnobCv()`/`LevelCv()`, the only two places a CV meets a panel
control. Do not "fix" this by rescaling the knobs — that bakes the jack's bias
into the panel.

Knobs 2–5 (+ their CVs) are smoothed by a PLAIN one-pole in the engine
(`UpdateBlock`, the `sh_` set, at the band-gain time constant). These land on
partial frequencies, so raw ADC noise there is FM at the callback rate (~2 cents
per LSB with the window collapsed onto one cluster). Smoothing lives in the
engine, not the hardware seam, so the emulator gets it too. What must NOT come
back is the backlash GATE (`archives.md`): it converts dense tiny noise into
sparse larger steps, which clicks. A plain smoother does the opposite. Pitch
(knob 1 / V-oct) is left
unsmoothed on purpose: smoothing it would lag V/oct tracking, and pitch noise is
common-mode vibrato, far less audible than the shaper's differential FM.

## Design rationale

**Sliders draw the spectrum.** Bands are geometric with width
`kNumPartials^(1/kNumBands)` — exact octaves when bands = log2(partials) (8 bands
= octaves at 256). Constant-Q is what matters perceptually; octaves are just a
round number. Each slider is a breakpoint of a piecewise-linear envelope in
log-frequency. A slider owns the partials from its own position up to the next
slider's, with **no crossfade into the neighbour in Cluster** — the comb picks
partials strictly per band, so blending the gain across the boundary meant a
band's survivors drew their level from the slider next door, and a band set to
highpass kept sounding with its own slider down. Shepard keeps a narrow
crossfade (`kXfadeW` of a band, linear): there partials slide *through* the
envelope, and a step at a boundary ticks once per crossing. It is Shepard-only
because `shepard` is loop-invariant and GCC unswitches it, so Cluster — the
worst-case mode — carries none of it.

**Sliders also control fill.** Over the bottom half a slider is level alone,
with one partial per band sounding; above half it fills the band in. So all
sliders at half is a skeleton comb of eight partials, and pushing them up adds
harmonics one at a time. Level and count are deliberately coupled: a band with
more partials in it *is* louder, and the envelope stays drawable at any level.
Pots 4–9 choose the fill *order* — which partial the band grows out from —
measured in **partials, not log-frequency**, so a pot means the same thing in a
band holding two and one holding thirty-two. Highpass sits at CCW so building a
hump is one diagonal gesture: a band left of the peak wants to keep the partials
nearest the peak, which are its high ones.

**Brightness** is mostly the sliders — pulling the top ones down *is* the
lowpass, with per-band resolution and a CV jack each. The global tilt (GATE)
is a coarser thing underneath that: which slope the untouched spectrum has.

**Inharmonicity** (slider 1 + CV) is the stiff-string model
`f_k = k·f0·√(1+B·k²)` — piano-ish low, bell/metallic high — the one thing a
filter fundamentally cannot fake. The k² law keeps low partials nearly pure on
its own.

**Fine tune** (pot 2, under the fundamental's own band) adds ±1 semitone to the
pitch; knob 1's 6.6-octave sweep is far too coarse to tune by hand. The firmware
snaps a small centre deadzone so 12 o'clock is exactly in tune despite pot
noise; the emulator pot is exact and has none.

**Spectral shift** (pot 1, under slider 1's inharmonicity — both bend the whole
spectrum) multiplies the coordinate the envelope and comb are indexed by, ±1
octave, without moving any partial's frequency. Only *which* partials are
chosen and how loud. Paired with the skeleton comb it scans the chosen harmonics
up and down the series. One multiply, because `sel` is the only thing either the
envelope or the comb is indexed by. `QUIRKS=1` gives it a two-hour software
centre detent for units whose pot has no mechanical one.

**Spectral tilt** (switch 2) morphs between `1/r` — a sawtooth's −6 dB/octave, what
an acoustic source does — and `1/√r`, which gives every octave equal power.
Harmonics are evenly spaced in frequency, so the octave above `r` holds `r`
partials; `1/√r` therefore sounds ~3 dB/octave brighter than a sawtooth, about
20 dB over the bank. That brightness makes every band slider equally effective,
which is a nice panel property, but it leaves the top octaves loud enough to
hear individual partials beating — exactly what a Shepard glide has to hide.
Implemented as a **lerp, not a branch**: `shepard` already splits the loop in
two and a second bool would make it four clones. Level-compensated (`tiltNorm`,
a quadratic fit to `sqrt(P(0)/P(m))`, worst error 0.06 dB), then `kDarkBoost`
deliberately puts the step back — see Gain staging.

**The shaper (knobs 2–5, switch 1)** retunes partials inside a movable window,
once per block. Partials outside stay anchored and hold the pitch while the ones
inside go weird; position and width are CV-able, so the window itself is
playable. Knobs 2–3 mean the same thing in both modes — that's what makes a
screenless panel survive the mode switch. Window edges are tapered (smoothstep
over `kWinEdge` = one partial, both modes): a hard edge clicks as sweeping knob
2/3 drags partials across it, and anything wider reaches partials the window is
supposed to leave alone.

- **Cluster** — `r_k = k·(1−density) + density·s_c` (s_c = the target inside k's
  cluster). Density is capped at 0.995; 0.9–1 is the useful beating zone.
  Knob 4 is bipolar about a dead centre where m = 1 and nothing shifts, matching
  Shepard so the panel means one thing: the **side** picks which end of a cluster
  its members land on. CW is the cluster's low end, a comb at multiples of m; CCW
  is its high end — the same comb offset by m−1, so it is no longer aligned to f0
  and rings instead of thickening. The offset is clamped to the window, so a
  single cluster wider than the window piles at the window's top edge instead of
  being shoved past Nyquist. Both anchors are one per-block add folded into the
  base the partial loop already sums, so the second direction costs the loop
  nothing.
  Partials-per-cluster soft-steps up to `2^(log2 N + 0.5)`, so the knob can reach
  a *single* cluster. The window taper sits **outside** the window (a partial at
  `winStart` is already fully shifted).
  - ⚠️ **The grid is anchored at `p0 = floor(winStart)`, an integer partial.**
    Off a fractional anchor, `m = 1` sends a partial to the grid point below it
    rather than to itself, so density detunes the whole window at knob 4's
    detent — the mode's neutral position stops being neutral. The integer anchor
    is also what lands a collapsed cluster on exact multiples of `m`. It must be
    the window start and not the taper's foot: anchored below `winStart` the
    partials under the taper form clusters of their own, down where the tilt
    makes them loud, and the clip guard goes past its knee.
  - **Density has a 2% deadzone at the bottom** (`kDensityDead`). The shift is
    `density · (m − 1)` partials, so at a wide cluster half a percent of leak —
    a pot that does not quite reach its end, or a stale CV null past
    `knobPlusCv`'s clamp — is already an audible detune. Cluster only; Shepard
    reads the same knob raw.

  `ClusterStart` clamps its `floor()` input at 0, below the anchor it would go
  negative and put the cluster start at `p − M`. (It returns the cluster START;
  the two soft-step groupings share one collapse and one crossfade instead of
  each carrying its own.) Cluster samples the envelope at the partial **index**,
  not the shifted frequency, so a slider always owns the same harmonics no matter
  where the collapse dragged them — sliders pick the *ingredients* in Cluster and
  *EQ the output* in Shepard, the same bargain knobs 4–5 already make. Indexing
  by partial number is also what makes the band lookup a table (see below).
- **Shepard** — `r_k = k + phi` inside the window. The band envelope is sampled
  at the *shifted* frequency, so at phi=1 the ensemble is identical to phi=0 and
  the wrap is seamless (measured flat to 0.17% over a phi sweep). Wide window +
  all sliders up = a Shepard tone, driven by the knob or by a ramp into its jack
  (`patch-book.md`). Narrow window = spectral shear on a slice. Knob 5 ducks the
  window contents, inverted so knob-at-rest = unity (a plain gain muted the whole
  window at rest, exactly where Cluster's density = 0 is neutral).
  - The bank is **finite**, so a shift by one index leaves nothing at the bottom
    and adds one past the top: the fundamental simply vanished as phi rose. The
    ends fade to silence by ratio (`kEndFade`), which is what makes the wrap
    exact rather than nearly: amplitude is a function of ratio alone, so the
    ensemble either side of a wrap is the same sound.
  - **The fade is `kEndFadeW` = 4 partials wide, not 1.** Any width keeps the
    wrap exact; the width is the low end's envelope. At 1 the newest partial is
    also the loudest in the bank under either tilt, so it arrives from nothing
    once per cycle and the glide reads as a *loop* rather than an endless rise.
    At 4 the cyclic swing of the low end is 0.09 dB and of the whole bank
    0.03 dB (against 0.91 and 0.34 at 1), at the price of ~8 dB of low end.
    A taste constant: smaller is a harder bottom.
  - **Phi's range doubles under ODD ONLY.** The wrap only closes when the
    surviving set lands back on itself: at phi=1 the odd set {1,3..95} sits on
    the EVEN positions {2,4..96}, so it jumps; at phi=2 it sits on {3,5..97},
    itself relabelled. Per block, so free.
  - **The rate tops out at 10 partials/s** (`kPhiRateMin`, `kPhiRateOct`). The
    illusion itself holds to roughly half a wrap per second: `k + phi` moves
    partial 1 a whole octave per wrap and partial 72 by 24 cents, so past that
    the ear tracks the individual sweeps instead of the ensemble. The top of the
    travel is therefore a zap effect rather than an endless rise — deliberate,
    but it means the usable Shepard range is the bottom of the knob.
  - **Phi is a position, not a knob reading** (`AdvancePhi`). The knob sets a
    *rate* in partials/s and the jack contributes its own *change*, folded so a
    step of more than half a wrap reads as a ramp resetting rather than as a
    sweep. Three things follow. An external ramp's reset is no longer a step, so
    the 5 ms shaper smoother cannot slew it into a downward glissando. The ramp
    no longer has to span exactly one partial: the fold makes up the difference,
    so the wrap always lands where the spectrum repeats. And there is no cable
    detection anywhere — an unpatched jack on this board is *grounded* through
    the Thonkiconn's own switch contact (verified in the PCB: pad 2 to GND, no
    GPIO), so "no cable" and "a source resting at 0 V" are the same reading and
    an envelope at rest is exactly the second one. Additive is the only honest
    combination.
  - **A wrap rolls the bank** (`RollPartials`). Amplitude per frequency is
    already continuous across a wrap, but phase and pan slot belong to the
    *partial*, not the frequency, so without the roll all 96 stepped phase at
    once — a click whose size did not depend on how carefully phi was aimed. The
    roll is a rotate of `phase_[]` plus an offset into the pan table, once per
    wrap at block rate; the render loop never sees it.
  - ⚠️ **The roll covers only `[moveLo, moveHi]`, the partials the window
    moves.** Re-phasing a partial that went nowhere is a step between two
    unrelated accumulators, and with the window above the fundamental that
    partial is the loudest in the bank. Restricting the roll takes a window from
    partial 2 from **54x** the median sample step to 5.7x, against a 4.5x floor.
    - **A phase step moves no gain, so no level metric can see it** — this
      survived a "0.00 dB swing over a cycle" measurement. Check a wrap with the
      first difference of the output, not with levels.
    - The pan roll stays global and is load-bearing: removing it puts the same
      patch back to 17.5x, because a moving partial needs its image to follow its
      frequency. Static partials do still change pan slot at a wrap, which is
      most of the 5.7x — the residual to attack first if this comes back.
  - **The window really excludes what it excludes, and the fade follows it.**
    Two things that only work together: the one-partial shoulder, and the window
    start and width snapping to whole partials on a `roll`-partial grid so ODD
    ONLY lands on odd partials. Together they put every partial's window weight
    at exactly **0 or 1** — a weight strictly between them is what breaks a
    windowed wrap, since such a partial moves a fractional step and lands
    nowhere. The ends fade keys to `[max(winStart,1), min(winEnd, kEndFade)]`
    rather than to the bank, so the moving region maps onto itself wherever it
    sits; wide open those two values *are* 1 and 97, so the flagship patch is
    unchanged. Measured on a window from partial 5, the whole-bank swing over a
    cycle is **0.00 dB** (0.87 dB keyed to the bank, where a hole opened at the
    window's own bottom edge once per cycle) with the low end 3.9 dB louder.
    - The cost is a spectral notch: the lowest moving partial is silent by
      construction, so there is a `kEndFadeW`-wide dip between the static bottom
      and the glide. It is static in time — a gap, not a loop.
  - It is *not* a Risset glide: `k + phi` moves partial 1 by an octave and
    partial 72 by 24 cents. What earns the name is the wrap, not uniform motion.
    A canonical Shepard is octave-spaced with no fundamental to fuse on; this is
    a harmonic bank whose fused pitch stays put while the partials slide through
    a fixed envelope. Pulling the low bands down is what moves it toward the
    classic percept — and in Shepard the envelope is frequency-indexed, so the
    sliders behave exactly like the Shepard bell they are standing in for.

**Random phase is unconditional**, not a switch: phases are seeded randomly at
init for crest factor (measured 2.8 vs 6.8 aligned — aligned clips at full
sliders). Phase is inaudible on a static spectrum (Ohm's law of acoustics), so
it never earned a panel control.

**The stereo image is a morph between configurations, not a width fader.** Pot 3
travels MONO → SCATTER → ORBIT (`kPanAnchor`) and then spends its last stretch on
the orbit's *motion* rather than its shape. A mono park holds the bottom 8% and
the three stages split what is left evenly. Width alone was
the original design and it wasted the knob: scaling one fixed fan makes every
intermediate a quieter version of the last, so only the two ends were worth
visiting.

- **The morph runs in position space, then converts once.** Two square roots per
  slot, 16 per block, regardless of how many anchors the table grows. Morphing
  in gain space instead would double that, and the per-block pass is the half of
  the budget that matters here.
- **New images are free.** The partial loop only ever reads `panDelta_[]`, 8 L/R
  pairs rebuilt per block; nothing about a new anchor reaches the hot path.
- **Every anchor must hold `kSlotPat`'s even-k/odd-k balance** or ODD ONLY shoves
  the image sideways. Asserted per anchor in `tests/slot_table.cpp`.
- **Nothing reaches a hard pan** except SCATTER's two end slots. Hard panning is
  what puts half the bank in one channel at full gain instead of all of it at
  0.707, and it is the only thing that ever moved a witness peak here. ORBIT's
  radius is 0.8 for exactly this reason, and the test asserts the ceiling.
- **ORBIT's offsets are not slot order, and its two subsets run at different
  rates.** Each parity subset gets four points 90° apart and its own phase
  accumulator, so both sums cancel at *every combination* of the two phases —
  which is what makes detuning safe. Slot order (`i/8`) leaves each subset
  summing to ~1.08 and the image leans as it turns. The test sweeps a 64×64 grid
  over both phases rather than trusting the algebra.
- At full CW the odd subset reverses and runs at `kPanRotCounter` × the even one.
  Near −1 the two clouds counter-rotate almost symmetrically and cross about
  twice per revolution; the small offset from −1 is what stops them locking, and
  sets how long the full pattern takes to come round (at −0.9918, ~122
  revolutions — about ten minutes at the full-CW rate).

*Considered and dropped* (Swarm mode, Pigments' Window/Warp modes and Shape
presets, a master-level pot, an inharmonicity-onset pot, anti-phase pan):
`archives.md`.

## Gain staging

Loudness follows power (Σa²), peak follows amplitude (Σa); they diverge by √N.
Chosen: **fixed headroom budget, no dynamic normalisation** — the Hammond
drawbar answer. Random initial phases (free ~9 dB of crest factor) and the tilt,
scaled so all-up lands around −19 dBFS RMS on the `1/r` slope. Power and peak
normalisation were both rejected (`archives.md`). `kHeadroom` rides in the
per-band gain, not the partial loop — it is a constant and everything it scales
flows through that gain anyway.

`kHeadroom` is **0.42**, sized so the loudest patch the panel can reach lands at
0.832 against the 0.85 knee — 0.2 dB spare. There is deliberately no room left:
anything that makes a patch louder now trips `clip_guard`.

`kDarkBoost` (**1.41**, +3 dB) rides on top of `tiltNorm`, faded out across the
morph so the bright end keeps the level the tilt fit gives it. Dark peaks ~3.5 dB
below bright at equal RMS — fewer loud high partials to align — so it has
headroom bright does not. Spending it makes dark ~2.5 dB louder than bright in
RMS: a deliberate level step on GATE, taken because the extra lands in the low
end. `tiltNorm` alone would keep the two matched.

Together these lifted the median patch from **−20.9 to −15.1 dBFS**. Note what
the budget is sized against: all eight sliders at maximum. A bank with random
phases sums as √N, so an ordinary two- or three-band patch still sits 4–5 dB
below the case being protected. Going further means accepting that the all-up
patch engages the soft clip, which changes what `clip_guard` asserts — and the
patches that cross first are not exotic: at +2 dB more it is `cluster, 8 sliders
up, dark, f0=55, no cluster at all`, the plain drawbar patch.

Measured worst case over 22,875 patches: **peak 0.594 against the 0.85 knee**,
at `cluster cfg2 f0=220 A=0.00 B=0.75 pos=0.75 win=1.00`. The hot corner is the
collapse-UP side of knob 4 (A near 0) with a wide window. Still inside
`kHeadroom`'s stated worst case (~0.75), and the margin is 3.1 dB — check this
number, not just pass/fail, if the collapse geometry changes again.

Which tilt is loudest depends on the compensation, so check rather than assume:
uncompensated, dark is far worse (2.005 vs bright's 1.208), but compensated the
worst case is **bright**. "Brighter must be louder" is not a safe reading in
either direction.

Cluster at high density is the loud exception: tilt and band envelope are
sampled at the SHIFTED frequency, so collapsing the bank down-spectrum adds
real power (~13 dB at full density). A **control-only** compensation now handles
it (`norm` in `UpdateBlock`, folded into the per-band gains). It is derived from
the density/partials/window geometry ALONE — never from engine amp state, which
is the structure that made two earlier attempts throw hardware-only ~8 kHz
artifacts (`archives.md`; do not build a gain that feeds amp state back into
`master`). RMS holds flat within 0.9 dB across the density knob.

Both closed forms are really over the interval the members end up occupying,
`[a, a+y]` in units of the window start. Collapsing down leaves `a = 1`;
collapsing up slides the same interval to the cluster's top, `a = 1+x−y`, where
either tilt makes it quieter — miss that and the up side over-boosts by up to
`(1+x)`. Both reduce to the plain forms at `a = 1`, and to 1 at density 0 where
`y = x` and nothing has moved.

⚠️ **`norm` may only attenuate** (`min(1/√boost, 1)`). `boost < 1` is the
collapse-UP direction, where the model asks for gain — several times unity at
max cluster, which takes a wide window past the clip. The request is wrong for
the partials outside the window in any case, since they never collapsed. Capping
it is also what buys the one-partial window shoulder in Cluster: with the wide
proportional taper the low partials were only partly collapsed, and narrowing it
without the cap puts the sweep's worst peak at 1.157. Measured across the density
knob with the cap: 0.4 dB collapsing up, exactly 0.00 dB at the centre detent.

⚠️ **Measure high density over seconds, not milliseconds.** A collapsed
cluster's members sit `(1−d)·f0` apart and beat with a multi-second period, so a
quarter-second RMS window reports whatever phase it happened to catch — it made
the up side look 8 dB worse than it is. `clip_guard` has an 8 s corner for this
reason; any new harness needs the same.

⚠️ **The compensation's closed form depends on the tilt exponent.** Integrating
`r^-2a` across a collapsed cluster gives `ln(1+t)/t` at `a = ½` but `1/(1+t)` at
`a = 1`, so each slope needs its own ratio and the two blend with the tilt morph.
If the tilt law ever changes, re-derive this: a compensation solving the wrong
spectrum swings the density knob by several dB, and the failure is silent apart
from that one test.

**Output clip: linear below a 0.85 knee, parabolic to a hard cap at 1.15.**
Below the knee the output stage is bit-exact, and as shipped nothing in the
guard's sweep reaches the knee at all — the clip is insurance, not a stage the
engine normally uses.

Partials above Nyquist fade out over the top 5% rather than switching off; a
band whose partials have all run off pulses its LED dimly (see below). At 48 kHz
the harmonic ceiling is real: f0 = 220 Hz leaves ~109 partials, 880 Hz only 27.

## Engine

**One shared sine LUT + N independent fixed-point phase accumulators.** Not
IFFT-to-wavetable (retunes only by rebuilding the table; bursty — whinebug
forbids it), not resonators (retuning needs transcendentals per partial per
block; the shaper retunes continuously). Per sample: phase add (uint32, free
wrap), one table read, two MACs. Per block (9.6 kHz): the whole per-partial
control pass — ratios, band lookup, tilt, Nyquist fade — which **dominates the
cost** (~3× the render loop; verify with objdump before optimising anything).

Table: 16384 × f32 (64 KB), **no interpolation** — phase truncation at 14 bits
is ≈ −84 dBc, far under this board's floor; the interpolator cost more than the
bigger table. Everything (state + table, ~66 KB) lives in **DTCM** via
`DTCM_MEM_SECTION`: uncached, zero wait state, constant-current — the quiet
choice. Default `.bss` is cached AXI SRAM, whose refills burst. Verify placement
after linker changes: `arm-none-eabi-nm build/Fox-Tail.elf | grep -w osc` →
`0x2000xxxx`.

### CPU (measured, -O3, block 5, H750)

Budget must fit the **worst-case mode = Cluster** (~0.52%/partial over ~31%
fixed; Shepard ~0.43%/partial):

| partials | Shepard | Cluster |
|---|---|---|
| 96 | ~72% | ~80% |
| 128 | ~89% | **overruns** |

**96 is the shipping value.** Overload does not merely glitch audio: the audio
IRQ starves the ADC (CV reads freeze), USB serial, and the LEDs. Headroom keeps
the *controls* alive. If more partials are ever wanted, the lever is Cluster's
per-partial math (incremental cluster-start instead of `floor()`), not the
render loop.

The two clones are **not** the same size, and `loop_cost.py` reports only the
larger. Cluster is currently 106 instructions and Shepard 123, so the headline
number tracks Shepard while the CPU budget is set by Cluster. To read them
apart, look for `vrintm` — that is `ClusterStart`'s `floor()`, and only the
Cluster clone has it.

### Costing a change before flashing it

`python3 tools/loop_cost.py` disassembles the engine for the H750 with the
firmware's flags and counts the innermost loops; `--update` rewrites
`tools/loop_cost.json`, `--root DIR` measures a prototype tree instead. Baseline:
**189 instructions per partial per block** (render 15 x5 frames + block loop
114) — reported against the larger clone, see the note above. Against the table
above that prices one block-loop instruction at ~0.26%
CPU and one render-loop instruction at ~1.3% — so **2 block-loop instructions
cost the same as one partial**. It derives 0.51%/partial from the disassembly,
which is the measured 0.52% arrived at independently.

Static instructions, not cycles: it cannot see VSQRT's 14 unpipelined cycles or
memory stalls, so treat the CPU figure as a ratio, not a reading. The hardware
meter stays the ground truth.

### Hard-won rules (each cost a debugging session)

1. **No libm in the per-partial path.** `std::sqrt` emits VSQRT (14 unpipelined
   cycles) *plus* a `bl sqrtf` errno fallback unless the build sets
   `-fno-math-errno -fno-trapping-math` (foxtail's Makefile does; the delay
   stays stock `-Os`).
2. **No FP ternaries in hot paths** — they compile to `vcmpe`+`vmrs` pipeline
   stalls; `std::fminf/fmaxf` give single-instruction `vminnm/vmaxnm`. Removing
   them was worth ~12 CPU points.
3. **No branches in the per-partial loop** — GCC clones the loop body per path.
4. **No sub-rate tricks** (half-rate control updates, round-robin partial
   updates): whinebug rule 2 — any periodic structure below the callback rate
   becomes an audible comb.
5. **Estimates lie; objdump doesn't.** Per-partial cost was mis-attributed
   three times by reasoning from C; the disassembly settled it each time.
6. **Ask what a line actually depends on.** `FastLog2(sel)` was the single most
   expensive line in the loop at 13–15 instructions. In Cluster `sel` is
   `idx * bandShift`, so `log2(sel)` is `log2(k+1) + log2(bandShift)` and the
   first term depends on nothing a control can move — it was recomputing the
   same 96 numbers 9,600 times a second. As a table built at `Init` it is a load
   and an add, and it took Cluster from 119 to 106 on its own. Dump the
   per-source-line instruction counts before reaching for `kNumPartials`; the
   caveat is that `FastLog2(a)+FastLog2(b)` is not bit-identical to
   `FastLog2(ab)`, so band placement shifts by a hair (smooth, monotonic,
   inaudible under the comb and gain smoothing — but it is a change, not a
   refactor).

## LED behaviour

LED 1 = inharmonicity. LEDs 2–9, two views, all main-loop (zero audio cost):

- **Level view** (default): brightness = band gain. A band whose slider is up
  but whose partials have all passed Nyquist **pulses dimly** — "turned down"
  and "nothing left to play" must not look identical.
- **Window peek**: moving knob 2 or 3 flips the row to showing the shaper
  window (edges partially lit) until 1.5 s after the knob stops.

`FOXTAIL_LED_MODE` in FoxTail.cpp: 1 = CPU load bar, 2 = switch-polarity check.

## Status

Hardware-verified end to end: engine, all 28 controls, both switches, per-band
CV, V/oct (calibrated to a few cents over 2 octaves — see CLAUDE.md for the
procedure and the power-supply gotcha), LEDs, serial diagnostics. The comb, the
fill-order pots, the global spread, the spectral shift, Cluster-by-index and the
GATE tilt are all hardware-confirmed, with Cluster measured at ~78% CPU.

The tilt sits on switch 2 and parity on GATE, swapped deliberately: parity's
"off" state (ALL partials) is genuinely neutral, so the jack costs nothing left
unpatched, while the tilt has no neutral and needed a control you can see and
leave set. Open items are in `todo.md`.
