// sources.js — the CV "input source" cards (depth + Off/Osc/Env, per jack).
// A card's controls send to the emulator as "<target>.cv.*". Wired targets:
// the pitch card and every slider CV. Uses shared helpers from app.js
// (send, saveVal, span, rowEl) and the model from labels.js (FT.model).
(function () {
  // ---- All source-slider ranges live here, in one place. --------------------
  // Times (atk/dec) are a normalized 0..1 position mapped to seconds by the
  // emulator's normToSec() (0->1ms, 1->10s); osc freqs are in Hz.
  const RANGES = {
    depth:  { min: 0,    max: 1,     step: 0.01,  def: 0.5 },
    coarse: { min: 0,    max: 20000, step: 1,     def: 0   }, // Hz (0..20 kHz)
    fine:   { min: 0.02, max: 20,    step: 0.01,  def: 1   }, // Hz (LFO)
    atk:    { min: 0,    max: 1,     step: 0.001, def: 0.05 },
    dec:    { min: 0,    max: 1,     step: 0.001, def: 0.623  }, // default 310 ms
    curve:  { min: -1,   max: 1,     step: 0.01,  def: 0    },
  };

  // ---- value readouts -------------------------------------------------------
  function secText(v) {                       // matches normToSec() in main.cpp
    const s = 0.001 * Math.pow(10000, v);
    return s < 1 ? `${Math.round(s * 1000)} ms` : `${s.toFixed(2)} s`;
  }
  const curveText = (v) => (v < -0.1 ? "log" : v > 0.1 ? "exp" : "lin");
  const freqText = (v) => (v >= 1000 ? `${(v / 1000).toFixed(2)} kHz` : `${v.toFixed(2)} Hz`);

  // ---- card building --------------------------------------------------------
  function rangeCtl(role, text, valText) {
    const R = RANGES[role];
    const lab = document.createElement("label");
    lab.textContent = text;
    const inp = document.createElement("input");
    inp.type = "range";
    inp.min = R.min; inp.max = R.max; inp.step = R.step; inp.value = R.def;
    inp.dataset.role = role;
    inp.addEventListener("mouseenter", () =>
        status(`${text}: ${inp.value}`)
    );
    const r = rowEl("ctl", lab, inp);
    if (valText !== undefined) r.append(span("val", valText));
    return r;
  }
  function segCtl() {
    const lab = document.createElement("label");
    lab.textContent = "Source";
    const seg = document.createElement("div");
    seg.className = "seg";
    [["off", "Off", true], ["osc", "Osc", false], ["env", "Env", false]].forEach(
      ([src, txt, on]) => {
        const b = document.createElement("button");
        b.textContent = txt;
        b.dataset.src = src;
        if (on) b.classList.add("on");
        seg.append(b);
      }
    );
    return rowEl("ctl", lab, seg);
  }

  function makeCard({ title, note, target, wired }) {
    const card = rowEl("incard");
    card.dataset.source = "off";
    const top = rowEl("incard-top", span("incard-title", title));
    if (note) top.append(span("incard-target", note));
    card.append(top);
    card.append(rangeCtl("depth", "Depth"));
    card.append(segCtl());

    // Osc: coarse + fine frequency (summed & clamped by the emulator).
    const osc = rowEl("src src-osc",
      rangeCtl("coarse", "Coarse", freqText(RANGES.coarse.def)),
      rangeCtl("fine", "Fine", freqText(RANGES.fine.def)));
    osc.hidden = true;

    // Env: attack/decay/curve + a Cycle checkbox + Trigger (becomes Stop).
    const env = rowEl("src src-env",
      rangeCtl("atk", "Attack", secText(RANGES.atk.def)),
      rangeCtl("dec", "Decay", secText(RANGES.dec.def)),
      rangeCtl("curve", "Curve", "lin"));
    env.hidden = true;
    const cyc = document.createElement("input");
    cyc.type = "checkbox";
    cyc.className = "cycle-box";
    const cycLab = document.createElement("label");
    cycLab.textContent = "Cycle";
    env.append(rowEl("ctl", cycLab, cyc));
    const trig = document.createElement("button");
    trig.className = "trig";
    trig.textContent = "Trigger";
    env.append(trig);

    card.append(osc, env);
    if (wired && target) wireCard(card, target);
    return card;
  }

  // Wire a card's controls to the emulator (id scheme "<target>.cv.*").
  function wireCard(card, target) {
    card.dataset.wiredTarget = target;
    const idFor = {
      depth: `${target}.cv.depth`,
      atk: `${target}.cv.env.attack`,
      dec: `${target}.cv.env.decay`,
      curve: `${target}.cv.env.curve`,
      coarse: `${target}.cv.osc.coarse`,
      fine: `${target}.cv.osc.fine`,
    };
    card.querySelectorAll("input[type=range]").forEach((inp) => {
      const id = idFor[inp.dataset.role];
      if (!id) return;
      inp.dataset.persistId = id; // so restoreState can find it
      inp.addEventListener("input", () => {
        const v = parseFloat(inp.value);
        const val = inp.parentElement.querySelector(".val");
        send(id, v);
        saveVal(id, v);
        if (val) {
          const role = inp.dataset.role;
          if (role === "atk" || role === "dec") val.textContent = secText(v);
          else if (role === "curve") val.textContent = curveText(v);
          else if (role === "coarse" || role === "fine") val.textContent = freqText(v);
        }
      });
    });

    // Cycle checkbox + Trigger/Stop button.
    const cyc = card.querySelector(".cycle-box");
    const trig = card.querySelector(".trig");
    const relabel = () => { if (trig) trig.textContent = cyc && cyc.checked ? "Stop" : "Trigger"; };
    if (cyc) {
      cyc.dataset.persistId = `${target}.cv.env.cycle`;
      cyc.addEventListener("change", () => {
        const v = cyc.checked ? 1 : 0;
        send(`${target}.cv.env.cycle`, v);
        saveVal(`${target}.cv.env.cycle`, v);
        relabel();
      });
    }
    if (trig) trig.addEventListener("click", () => {
      if (cyc && cyc.checked) { cyc.checked = false; cyc.dispatchEvent(new Event("change")); } // Stop
      else send(`${target}.cv.env.trig`, 1);                                                   // one-shot
    });
    relabel();
  }

  // Main CV panel: one card per param jack (+ GATE). Pitch is wired.
  function buildParamCards() {
    const M = FT.model;
    const box = document.getElementById("cv-cards");
    if (!box || !M) return;
    box.innerHTML = "";
    Object.keys(M.ctrl.knobs).forEach((param) => {
      const jackT = M.ctrl.cvInputs.params[param];
      box.append(makeCard({
        title: jackT ? `${M.labelOf(jackT)} CV` : `${param} CV`,
        note: `→ ${M.labelOf(M.ctrl.knobs[param])}`,
        target: param,
        wired: param === "pitchHz",
      }));
    });
    const gate = rowEl("incard is-empty",
      rowEl("incard-top", span("incard-title", M.labelOf(M.ctrl.cvInputs.gate)),
            span("incard-target", "→ sync/trig")),
      span("src-empty", "digital gate — TBD"));
    box.append(gate);
  }

  // Slider CV panel: one card per slider; each source sums into its slider.
  function buildSliderCvCards() {
    const M = FT.model;
    const box = document.getElementById("slider-cv-cards");
    if (!box || !M) return;
    box.innerHTML = "";
    Object.entries(M.ctrl.sliders).forEach(([id, t]) =>
      box.append(makeCard({ title: `${M.labelOf(t)} CV`, target: id, wired: true }))
    );
  }

  // Keep card titles in sync when labels change (called from applyLabels).
  function refreshTitles() {
    const M = FT.model;
    if (!M) return;
    const cols = Object.values(M.ctrl.sliders).map(M.labelOf);
    document.querySelectorAll("#slider-cv-cards .incard-title").forEach((el, i) => {
      if (cols[i] !== undefined) el.textContent = `${cols[i]} CV`;
    });
    const cvCards = document.querySelectorAll("#cv-cards .incard");
    const params = Object.keys(M.ctrl.knobs);
    params.forEach((param, i) => {
      const card = cvCards[i];
      if (!card) return;
      const jackT = M.ctrl.cvInputs.params[param];
      card.querySelector(".incard-title").textContent = jackT ? `${M.labelOf(jackT)} CV` : `${param} CV`;
      const note = card.querySelector(".incard-target");
      if (note) note.textContent = `→ ${M.labelOf(M.ctrl.knobs[param])}`;
    });
    const gateCard = cvCards[params.length];
    if (gateCard) gateCard.querySelector(".incard-title").textContent = M.labelOf(M.ctrl.cvInputs.gate);
  }

  // One delegated handler drives every Off/Osc/Env picker (all cards).
  function attachSegHandler() {
    document.addEventListener("click", (e) => {
      const btn = e.target.closest(".seg button[data-src]");
      if (!btn) return;
      btn.parentElement.querySelectorAll("button").forEach((b) =>
        b.classList.toggle("on", b === btn)
      );
      const card = btn.closest(".incard");
      if (!card) return;
      card.dataset.source = btn.dataset.src;
      card.querySelectorAll(".src").forEach((s) => {
        s.hidden = !s.classList.contains("src-" + btn.dataset.src);
      });
      if (card.dataset.wiredTarget) {
        const v = { off: 0, osc: 1, env: 2 }[btn.dataset.src];
        send(`${card.dataset.wiredTarget}.cv.src`, v);
        saveVal(`${card.dataset.wiredTarget}.cv.src`, v);
      }
    });
  }

  window.FT = window.FT || {};
  FT.sources = { buildParamCards, buildSliderCvCards, attachSegHandler, refreshTitles };
})();
