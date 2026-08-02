# The high-pitched whine — findings

Investigation closed. The evidence (spectrum attribution, the OG comparison, the
rejected round-robin scan) is in `archives.md`; this is the part that still
constrains code.

## Summary

The whine is **not** the MCU clock. It is the CPU's rhythmic, bursty power draw
coupling into the analog audio path. The chip wakes once per audio block, does a
burst of work, and idles; a burst repeating thousands of times per second is an
audible tone. Anything that changes the rhythm moves the pitch (block size 7 →
~6.9 kHz, block size 5 → ~9.6 kHz); anything that changes the burst's size
changes its loudness.

Three contributors, measured separately: the delay's DSP + external-RAM
streaming, the per-callback control-scan burst, and a hardware floor at ~12 kHz
that the stock OG Time Machine shows too. With DSP and ADC scanning both
stopped, this board measures as quiet as the OG — so the floor is reachable.

## Verdict

- **Delay firmware: leave it stock.** It is within a hair of its achievable
  optimum. The remaining gap vs the OG is the workload plus the DIY carrier
  board's coupling — a hardware problem, not a software one.
- **A lightweight firmware (additive oscillator) is OG-quiet even on this
  board** — proven by the near-idle measurement, and since confirmed by Fox
  Tail. Its residual ~9.6 kHz tone is rule 3 below, not a new bug.

## Design rules for new firmware (learned the hard way)

1. No external-RAM streaming.
2. Constant work per callback — no data-dependent spikes, and no periodic
   structure slower than the callback rate (any sub-rate pattern becomes an
   audible comb at the pattern rate). This is why round-robin control scanning
   was tried and rejected; do not re-attempt it.
3. The one unavoidable spur sits at the callback rate: place it high (block size
   ≤ 5 at 48 kHz → ≥ 9.6 kHz) where the ear is least sensitive.
4. Don't pass audio input through unless needed — the input stage picks up the
   whine and any buffer/replay path re-amplifies it.
