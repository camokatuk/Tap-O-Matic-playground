# The high-pitched whine — findings

## Summary

The whine is **not** the MCU clock. It is the CPU's rhythmic, bursty power
draw coupling into the analog audio path. The chip wakes once per audio block,
does a burst of work, and idles; a burst repeating thousands of times per
second is an audible tone. Anything that changes the rhythm moves the pitch
(block size 7 → ~6.9 kHz whine; block size 5 → ~9.6 kHz), and anything that
changes the burst's size changes its loudness.

## Attribution (measured, block size 5, output boosted +35 dB, sliders down)

Isolated by A/B firmware tests: mute the delay DSP, stop the control ADC
scanning, or both, and compare spectra.

| Spectrum component | Source | Fixable in software? |
|---|---|---|
| Tone forest below 9.6k (~1.7, 2.4, 4.8, 7.2, 7.9 kHz) + white-noise floor elevated ~7 dB vs OG | Delay engine DSP + external-RAM streaming | Only by removing the workload (an oscillator firmware would have none of this) |
| 9.6 kHz + 19.2 kHz | Per-callback control-scan burst (~50 muxed inputs read + processed in one thump per block) | Yes, but not worth it (see below) |
| ~12 kHz + quiet ~10 kHz | Hardware floor. The stock OG Time Machine shows the same signature (12k + one quiet tone) | No |

With DSP muted **and** ADC scanning stopped (chip near idle), the Tap-O-Matic
measures as quiet as the OG. That is the floor, and it is reachable.

Ruled out: LEDs (`setLeds = false` changed nothing), raw clock frequency.

## Extra finding: the delay amplifies the whine

The whine also couples into the codec **input** stage, gets recorded into the
delay buffer, and is re-mixed by the taps. Evidence: on the OG, individual
sliders can make its 12k tone quieter (phase cancellation between delayed
copies of a coherent sine); at minimum time all taps sum in phase and the
whine jumps out.

## Round-robin control scanning: tested and rejected

Spreading the control scan across callbacks (one handler group per block
instead of one big burst, slew-compensated) was implemented and measured:

- A fixed scan order repeats at callbackRate/14 ≈ 686 Hz and produced a comb
  of its harmonics (prominent 6.8 kHz spur = 10th harmonic). Interestingly the
  stock 12k peak partly disappeared — it was partly an intermod product
  (9.6k + 2.4k).
- Shuffling the order each cycle (spread-spectrum style) did not remove the
  comb: the rigid 14-block cycle still concentrates energy at frame-rate
  harmonics.
- **Subjectively worse than stock.** Ear sensitivity peaks at 2–5 kHz and
  falls steeply above ~10 kHz. Stock parks its spurs at 9.6/12/19 kHz — the
  least audible placement available. Any scan-spreading relocates energy
  downward into more audible territory.

The change was rolled back. The small block size (5) inherited from Eris's
tweaks is accidentally near-optimal psychoacoustics for this firmware.

## Why the OG is quieter: it's the board, not the code

The OG runs the same firmware lineage — same 150 s buffer, 9 taps,
external-RAM streaming, similar control scanning — yet shows only the
hardware-floor tones (no spur at its ~6.9 kHz callback rate). Meanwhile a
near-idle Tap-O-Matic matches the OG's spectrum. Conclusion: both boards
generate the same digital noise; **the Tap-O-Matic's redesigned DIY carrier
board couples far more of it into the audio path** (power filtering / ground
routing / layout). Software can shrink the noise source but cannot fix the
leak. Making the *delay* OG-quiet would take a hardware revision (KiCad files
are in the repo; suspects: supply filtering, decoupling, ground plane).

Note: shrinking the buffer (150 s → 30/60 s) would NOT help — the whine scales
with memory traffic per second, which is independent of buffer length.

## Verdict

- **Delay firmware: leave it stock.** It is within a hair of its achievable
  optimum; the remaining gap vs the OG is inherent to the workload plus the
  board's coupling.
- **A lightweight firmware (additive oscillator) will be OG-quiet even on
  this board** — proven by the near-idle measurement.

## Design rules for new firmware (learned the hard way)

1. No external-RAM streaming.
2. Constant work per callback — no data-dependent spikes, and no periodic
   structure slower than the callback rate (any sub-rate pattern becomes an
   audible comb at the pattern rate).
3. The one unavoidable spur sits at the callback rate: place it high
   (block size ≤ 5 at 48 kHz → ≥ 9.6 kHz) where the ear is least sensitive.
4. Don't pass audio input through unless needed — the input stage picks up
   the whine and any buffer/replay path re-amplifies it.
