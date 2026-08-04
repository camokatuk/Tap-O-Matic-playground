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
| Pot 3 | stereo spread width, 0 = mono | — |
| Pots 4–9 | fill *order* for bands 3–8: CCW highpass, centre bandpass, CW lowpass | — |
| Knob 1 (was TIME) | pitch, 20–2000 Hz log; its jack is calibrated V/oct | yes |
| Knob 2 (was SPREAD) | shaper window start | yes |
| Knob 3 (was FEEDBACK) | shaper window width | yes |
| Knob 4 (was HPF) | cluster: partials per cluster (soft-step) / shepard: phi | yes |
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
back is the backlash GATE: a gate + one-pole was tried, audibly clicked, and was
rolled back (`archives.md`) — it converts dense tiny noise into sparse larger
steps. A plain smoother does the opposite. Pitch (knob 1 / V-oct) is left
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
a quadratic fit to `sqrt(P(0)/P(m))`, worst error 0.06 dB) so the flip changes
colour and not loudness.

**The shaper (knobs 2–5, switch 1)** retunes partials inside a movable window,
once per block. Partials outside stay anchored and hold the pitch while the ones
inside go weird; position and width are CV-able, so the window itself is
playable. Knobs 2–3 mean the same thing in both modes — that's what makes a
screenless panel survive the mode switch. Window edges are tapered (smoothstep
over 10% of width): a hard edge clicks as sweeping knob 2/3 drags partials
across it.

- **Cluster** — `r_k = k·(1−density) + density·s_c` (s_c = start of k's
  cluster). Density is capped at 0.995; 0.9–1 is the useful beating zone.
  Partials-per-cluster soft-steps up to `2^(log2 N + 0.5)`, so the knob can reach
  a *single* cluster — at the old ceiling of 64 a second cluster start was
  stranded high in the spectrum and could not be removed. The window taper sits
  **outside** the window (a partial at `winStart` is already fully shifted);
  inside, a full-width window left the top ~9 partials at ~20 kHz whatever the
  other knobs did. `ClusterStart` clamps its `floor()` input at 0 — below
  `winStart` it would go negative and put the cluster start at `p − M`. (It
  returns the cluster START; the two soft-step groupings share one collapse and
  one crossfade instead of each carrying its own — cheaper, audio unchanged.)
  Cluster samples the envelope at the partial **index**, not the shifted
  frequency, so a slider always owns the same harmonics no matter where the
  collapse dragged them. With frequency-sampling, sweeping density emptied the
  top of the spectrum and the upper sliders went dead under your fingers. So
  sliders pick the *ingredients* in Cluster and *EQ the output* in Shepard —
  they mean different things per mode, which is the same bargain knobs 4–5
  already make. Indexing by partial number is also what makes the band lookup a
  table (see below).
- **Shepard** — `r_k = k + phi` inside the window. The band envelope is sampled
  at the *shifted* frequency, so at phi=1 the ensemble is identical to phi=0 and
  the wrap is seamless (measured flat to 0.17% over a phi sweep). Wide window +
  all sliders up + a ramp LFO into knob 4's CV jack = a Shepard tone as a patch
  (`patch-book.md`). Narrow window = spectral shear on a slice. Knob 5 ducks the
  window contents, inverted so knob-at-rest = unity (a plain gain muted the whole
  window at rest, exactly where Cluster's density = 0 is neutral).
  - The bank is **finite**, so a shift by one index leaves nothing at the bottom
    and adds one past the top: the fundamental simply vanished as phi rose. The
    outermost partials fade to silence by ratio (`kEndFade`), which makes the
    wrap exact rather than nearly — at phi=0 the sound is {2..96} plus a silent
    1, at full phi {2..96} plus a silent 97. One partial wide, so only two are
    ever touched and each fades across a whole sweep.
  - **Phi's range doubles under ODD ONLY.** The wrap only closes when the
    surviving set lands back on itself: at phi=1 the odd set {1,3..95} sits on
    the EVEN positions {2,4..96}, so it jumps; at phi=2 it sits on {3,5..97},
    itself relabelled. Per block, so free.
  - It is *not* a Risset glide: `k + phi` moves partial 1 by an octave and
    partial 72 by 24 cents. What earns the name is the wrap, not uniform motion.

**Random phase is unconditional**, not a switch: phases are seeded randomly at
init for crest factor (measured 2.8 vs 6.8 aligned — aligned clips at full
sliders). Phase is inaudible on a static spectrum (Ohm's law of acoustics), so
it never earned a panel control.

*Considered and dropped* (Swarm mode, Pigments' Window/Warp modes and Shape
presets, a master-level pot, an inharmonicity-onset pot): `archives.md`.

## Gain staging

Loudness follows power (Σa²), peak follows amplitude (Σa); they diverge by √N.
Chosen: **fixed headroom budget, no dynamic normalisation** — the Hammond
drawbar answer. Random initial phases (free ~9 dB of crest factor) and the tilt,
scaled so all-up lands around −19 dBFS RMS on the `1/r` slope. Power and peak
normalisation were both rejected (`archives.md`). `kHeadroom` rides in the
per-band gain, not the partial loop — it is a constant and everything it scales
flows through that gain anyway.

`kHeadroom` is **0.30**, raised from 1/5 once the `1/r` tilt had cost ~5 dB and
left the worst patch peaking 0.497 against a 0.85 knee. Note what it is sized
against: all eight sliders at maximum. A bank with random phases sums as √N, so
an ordinary two- or three-band patch sits 6–9 dB below the case being protected
— every normal patch pays for one nobody plays. Raising it further is a real
option, but it means accepting that the all-up patch engages the soft clip,
which changes what `clip_guard` asserts.

Measured worst case over 37,050 patches: **peak 0.613 against the 0.85 knee**,
at `cluster BRIGHT f0=220 A=0.12 B=0.88 pos=0.50 win=0.75`.

Which tilt is loudest depends on the compensation, so check rather than assume:
uncompensated, dark is far worse (2.005 vs bright's 1.208), but once the Cluster
compensation was re-derived per tilt the compensated worst case moved to
**bright**. Both facts have been measured at different points in the same
session and the naive reading ("brighter must be louder") was wrong the first
time and right the second, for reasons that had nothing to do with brightness.

Cluster at high density is the loud exception: tilt and band envelope are
sampled at the SHIFTED frequency, so collapsing the bank down-spectrum adds
real power (~13 dB at full density). A **control-only** compensation now handles
it (`norm` in `UpdateBlock`, folded into the per-band gains). It is derived from
the density/partials/window geometry ALONE — never from engine amp state, which
is the structure that made two earlier attempts throw hardware-only ~8 kHz
artifacts (`archives.md`; do not build a gain that feeds amp state back into
`master`). RMS holds flat within 0.9 dB across the density knob.

⚠️ **The compensation's closed form depends on the tilt exponent.** Integrating
`r^-2a` across a collapsed cluster gives `ln(1+t)/t` at `a = ½` but `1/(1+t)` at
`a = 1`, so each slope needs its own ratio and the two blend with the tilt morph.
Changing the tilt and leaving `Log1pOverX` alone left the compensation solving a
spectrum the engine no longer produced, and the density knob swung **6.4 dB**
instead of 0.9. If the tilt law ever changes again, re-derive this — the failure
is silent apart from that one test.

**Output clip: linear below a 0.85 knee, parabolic to a hard cap at 1.15.**
Below the knee the output stage is bit-exact; only Cluster crosses it, and only
42 of 4380 test patches engage it at all.

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
