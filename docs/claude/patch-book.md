# Fox Tail — patch book

Settings that produce a specific result, recorded so they don't have to be
rediscovered. Reference material only; nothing here constrains the code.

Panel numbering is physical: slider 1 is inharmonicity, sliders 2..9 are bands
0..7. Pot 1 is the spectral shift, pot 2 fine tune, pot 3 stereo spread, pots
4..9 the shapes of bands 2..7.

---

## The Shepard illusion

The mode is named after Arturia's, and is the same primitive: each partial
shifts toward the next one up (`r_k = k + phi`) inside a window. It is *not* a
Risset glide — `k + phi` moves partial 1 by an octave and partial 72 by 24
cents, so the low partials sweep enormously and the high ones barely move. What
earns the name is the wrap: at phi=1 the ensemble is identical to phi=0 with the
indices relabelled, so a repeating ramp never audibly restarts.

- Switch 1 **up** (SHEPARD)
- Knob 2 (position) fully **CCW** — window starts at partial 1
- Knob 3 (window) fully **CW** — window covers the whole bank
- Knob 5 (duck) fully **CCW** — unity gain
- **All sliders up.** A drawn hump does not work: sliders control fill as well
  as level, so turning the top band down thins it to one partial instead of
  fading it, and the upper harmonics mismatch. The engine fades the ends itself
  (see below), so the hump is not needed.
- **Ramp LFO into knob 4's CV jack**, knob near centre, moderate depth, a few
  seconds per cycle.

The LFO is the illusion. Without it there is only a static shifted spectrum.

Switch 2 works in either position: phi's range doubles under ODD ONLY, because
the odd set only lands back on itself after two index steps, not one.

The bank is finite, so the outermost partials are faded to silence by ratio
(`kEndFade`) — without that the fundamental audibly vanishes as phi rises and
nothing replaces it. Each of the two affected partials fades across a whole phi
sweep, which is why the wrap is exact rather than nearly exact.

## Skeleton comb

All sliders at half: one partial per band, eight in total, positions chosen by
pots 4..9 (CCW = the band's lowest partial, centre = its middle, CW = its
highest). Pot 3 then scans the whole shape up and down the harmonic series
without retuning anything.
