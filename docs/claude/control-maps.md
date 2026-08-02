# Fox Tail — control map & engine (living doc)

The instrument design: how the panel maps onto the partial bank, and the engine
that renders it. Companions: `oscillator-impl.md` (repo architecture),
`whinebug.md` (noise budget), `pigments-harmonic-engine.md` (the commercial
reference this borrows from). Everything below is implemented and
hardware-verified unless marked open.

## The panel

| Control | Function | CV? |
|---|---|---|
| Slider 1 | inharmonicity amount | yes |
| Pot 1 | fine tune, ±1 semitone (centre = in tune, clockwise = sharp) | — |
| Sliders 2–9 | gain of bands 1–8 | yes, 1 each |
| Pots 2–9 | pan of bands 1–8 | — |
| Knob 1 (was TIME) | pitch, 20–2000 Hz log; its jack is calibrated V/oct | yes |
| Knob 2 (was SPREAD) | shaper window start | yes |
| Knob 3 (was FEEDBACK) | shaper window width | yes |
| Knob 4 (was HPF) | cluster: partials per cluster (soft-step) / shepard: phi | yes |
| Knob 5 (was LPF) | cluster: density / shepard: window duck (CCW = unity) | yes |
| Switch 1 (left) | down = CLUSTER, up = SHEPARD | — |
| Switch 2 (right) | up = ALL partials, down = ODD ONLY | — |
| GATE | unassigned (digital only — cannot read analog CV) | — |
| LED 1 | inharmonicity amount | — |
| LEDs 2–9 | band levels / shaper-window peek | — |

Hardware facts (measured): switch 1 reads HIGH when down, switch 2 HIGH when up
(opposite polarities); pot 1 reads with opposite polarity to pots 2–9.

Knobs 2–5 (+ their CVs) are read RAW — do not re-add conditioning. A
backlash-gate + one-pole conditioner was tried (to stop Cluster's `floor()`
turning LSB noise into per-block frequency hops) and audibly clicked: it turns
dense tiny noise into sparse larger steps. With the window taper outside the
window, raw knob noise is inaudible (hardware A/B, 2026-08).

## Design rationale

**Sliders draw the spectrum.** Bands are geometric with width
`kNumPartials^(1/kNumBands)` — exact octaves when bands = log2(partials) (8 bands
= octaves at 256). Constant-Q is what matters perceptually; octaves are just a
round number. Each slider is a breakpoint of a piecewise-linear envelope in
log-frequency, sitting at its band's **centre** with a flat plateau in the
crossfade — edge-placed breakpoints made one slider light and sound like three.
The crossfade is also what keeps a partial's gain continuous as the shaper
glides it across a band edge.

**Brightness is not a knob** — pulling the top sliders down *is* the lowpass,
with per-band resolution and a CV jack each.

**Inharmonicity** (slider 1 + CV) is the stiff-string model
`f_k = k·f0·√(1+B·k²)` — piano-ish low, bell/metallic high — the one thing a
filter fundamentally cannot fake. The k² law keeps low partials nearly pure on
its own.

**Fine tune** (pot 1) adds ±1 semitone to the pitch; knob 1's 6.6-octave sweep
is far too coarse to tune by hand. The firmware snaps a small centre deadzone
so 12 o'clock is exactly in tune despite pot noise; the emulator pot is exact
and has none.

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
  other knobs did. `ClusterRatio` clamps its `floor()` at 0 — below `winStart` it
  goes negative and puts the cluster start at `p − M`.
- **Shepard** — `r_k = k + phi` inside the window. The band envelope is sampled
  at the *shifted* frequency, so at phi=1 the ensemble is identical to phi=0 and
  the wrap is seamless (measured flat to 0.17% over a phi sweep). Wide window +
  a hump drawn on the sliders + a ramp LFO into knob 4's CV jack = a true
  Shepard tone as a patch. Narrow window = spectral shear on a slice. Knob 5
  ducks the window contents, inverted so knob-at-rest = unity (a plain gain
  muted the whole window at rest, exactly where Cluster's density = 0 is
  neutral).

**Random phase is unconditional**, not a switch: phases are seeded randomly at
init for crest factor (measured 2.8 vs 6.8 aligned — aligned clips at full
sliders). Phase is inaudible on a static spectrum (Ohm's law of acoustics), so
it never earned a panel control.

*Considered and dropped:* Swarm mode (slider = one harmonic expanded into a
detuned cluster — no knob left for its spread, and Cluster near density 1 gives
the same beating); Pigments' Window/Warp shaper modes (more mode-dependent knobs
than the panel carries) and its Shape presets (hand-drawn sliders beat 12
presets); a master-level pot (fixed internal level instead); an
inharmonicity-onset pot (partial below which the series stays harmonic, cf.
Pigments' Modal Warp "Range" — at high settings only quiet/ultrasonic partials
bent, so the control read as dead; the k² law already protects the low end,
and the pot went to fine tune).

## Gain staging

Loudness follows power (Σa²), peak follows amplitude (Σa); they diverge by √N.
Chosen: **fixed headroom budget, no dynamic normalisation** — the Hammond
drawbar answer. Random initial phases (free ~9 dB of crest factor), `1/√r` tilt
(equal power per geometric band *and* a pink-ish all-up spectrum in one
multiply), scaled so all-up ≈ −16 dBFS RMS. Rejected: power normalisation
(raising one slider ducks the others — feels broken) and peak normalisation
(audibly quietens as partials are added). The loudest normal patch is **all
sliders up with the pans off centre** — a hard-panned band is +3 dB per channel
over a centred one; it peaks 0.73.

Cluster at high density is the loud exception: tilt and band envelope are
sampled at the SHIFTED frequency, so collapsing the bank down-spectrum adds
real power (~13 dB at full density), which lands on the clip. Two dynamic
compensations were tried and ROLLED BACK — both produced loud hardware-only
artifacts (see todo.md, the "8k harmonics" post-mortem). The open plan is a
static knob-position → gain table; until then the clip owns it.

**Output clip: linear below a 0.85 knee, parabolic to a hard cap at 1.15.** The
old cubic had no linear region — every patch wore ~−35 dB of always-on
waveshaping. Below the knee the output stage is now bit-exact; only Cluster
crosses it. `tests/run.sh` (grid + 1500 random patches, firmware block size):
42 of 4380 patches engage the clip, all Cluster at high density, worst −5.5 dB
residual; every non-Cluster patch is untouched.

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
procedure and the power-supply gotcha), LEDs, serial diagnostics. Open items:
GATE assignment, listening-driven tuning of ranges/curves, panel relabeling
(SVG editor in the emulator), optional switch-direction swaps before labels are
final.
