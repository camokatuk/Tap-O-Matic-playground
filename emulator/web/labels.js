// labels.js — the panel SVG owns all label text; this ties the emulator UI to
// it and lets you rename labels live. On load it fetches controls.json (control
// id -> tspan id) and GET /svg-labels (tspan id -> current text), then applies
// the SVG's labels across the UI. Editing a label and pressing Enter POSTs to
// /label, which rewrites that one tspan in the SVG and re-renders the panel PNG.
// Loaded after app.js.
(function () {
  let CTRL = null;   // controls.json
  let SVG = {};      // tspan id -> text

  const labelOf = (t) => (SVG[t] !== undefined ? SVG[t] : t);

  async function fetchSvgLabels() {
    SVG = await (await fetch("/svg-labels")).json();
  }

  function setData(id, label) {
    const el = document.querySelector(`[data-id="${CSS.escape(id)}"]`);
    if (el) el.dataset.label = label;
  }

  // Push the SVG's labels into the live UI (control tooltips, mirror rows...).
  function applyLabels() {
    const cols = Object.values(CTRL.sliders).map(labelOf); // column labels

    Object.entries(CTRL.sliders).forEach(([id, t]) => setData(id, labelOf(t)));
    Object.entries(CTRL.knobs).forEach(([id, t]) => setData(id, labelOf(t)));

    const pan = labelOf(CTRL.pots.groupLabel);
    CTRL.pots.ids.forEach((id, i) => setData(id, `${pan} ${cols[i]}`));

    Object.entries(CTRL.switches).forEach(([id, arr]) => {
      if (id.startsWith("_")) return;
      const el = document.querySelector(`[data-id="${CSS.escape(id)}"]`);
      if (el) el.dataset.states = arr.map(labelOf).join(",");
    });

    // Refresh the label text in app.js's "Unassigned" mirror rows.
    document.querySelectorAll("[data-unassigned]").forEach((el) => {
      const val = document.getElementById("mirror-" + el.dataset.id);
      if (val && val.previousElementSibling)
        val.previousElementSibling.textContent = el.dataset.label;
    });

    // Refresh editor inputs (leave the one you're typing in alone).
    document.querySelectorAll(".lbl-input").forEach((i) => {
      if (document.activeElement !== i) i.value = labelOf(i.dataset.tspan);
    });

    // Slider-CV card titles track their slider label ("<label> CV").
    document.querySelectorAll("#slider-cv-cards .incard-title").forEach((el, i) => {
      if (cols[i] !== undefined) el.textContent = `${cols[i]} CV`;
    });

    // Param-CV card titles/targets track the jack + knob labels.
    const cvCards = document.querySelectorAll("#cv-cards .incard");
    const params = Object.keys(CTRL.knobs);
    params.forEach((param, i) => {
      const card = cvCards[i];
      if (!card) return;
      const jackT = CTRL.cvInputs.params[param];
      card.querySelector(".incard-title").textContent = jackT ? `${labelOf(jackT)} CV` : `${param} CV`;
      const note = card.querySelector(".incard-target");
      if (note) note.textContent = `→ ${labelOf(CTRL.knobs[param])}`;
    });
    const gateCard = cvCards[params.length];
    if (gateCard) gateCard.querySelector(".incard-title").textContent = labelOf(CTRL.cvInputs.gate);
  }

  function reloadPanel() {
    const p = document.getElementById("panel");
    if (p) p.style.backgroundImage = `url("Fox-Tail.png?t=${Date.now()}")`;
  }

  async function rename(tspan, text) {
    const res = await fetch(
      `/label?tspan=${encodeURIComponent(tspan)}&text=${encodeURIComponent(text)}`,
      { method: "POST" }
    );
    if (!res.ok) return;
    await fetchSvgLabels();
    applyLabels();
    reloadPanel(); // PNG was re-rendered server-side
  }

  // ---- editor DOM helpers ----
  const span = (cls, text) => {
    const s = document.createElement("span");
    s.className = cls;
    s.textContent = text;
    return s;
  };
  const rowEl = (cls, ...kids) => {
    const d = document.createElement("div");
    d.className = cls;
    kids.forEach((k) => k && d.append(k));
    return d;
  };
  function labelInput(tspan) {
    const i = document.createElement("input");
    i.type = "text";
    i.className = "lbl-input";
    i.dataset.tspan = tspan;
    i.value = labelOf(tspan);
    i.addEventListener("change", () => rename(tspan, i.value));
    i.addEventListener("keydown", (e) => { if (e.key === "Enter") i.blur(); });
    return i;
  }
  function group(title) {
    const g = document.createElement("div");
    g.className = "lbl-group";
    g.append(span("lbl-group-title", title));
    return g;
  }

  function buildEditor() {
    const box = document.getElementById("labels-editor");
    if (!box) return;
    box.innerHTML = "";

    const head = rowEl("inputs-head");
    const h2 = document.createElement("h2");
    h2.textContent = "Panel labels";
    head.append(h2, span("badge", "edits SVG + re-renders"));
    box.append(head);

    const grid = rowEl("lbl-grid");

    // Sliders — individually editable.
    const gs = group("Levels");
    Object.values(CTRL.sliders).forEach((t, i) =>
      gs.append(rowEl("lbl-row", span("lbl-key", `Slider ${i + 1}`), labelInput(t)))
    );
    grid.append(gs);

    // Knob + its CV jack, two inputs side by side.
    const gk = group("Knobs + CV");
    gk.append(rowEl("lbl-row lbl-head", span("lbl-key", ""),
                    span("lbl-col", "knob"), span("lbl-col", "CV in")));
    Object.entries(CTRL.knobs).forEach(([id, t]) => {
      const cvT = CTRL.cvInputs.params[id];
      gk.append(rowEl("lbl-row", span("lbl-key", id),
                      labelInput(t), cvT ? labelInput(cvT) : span("lbl-na", "—")));
    });
    gk.append(rowEl("lbl-row", span("lbl-key", "gate"),
                    span("lbl-na", "—"), labelInput(CTRL.cvInputs.gate)));
    grid.append(gk);

    // Pots — one shared group label.
    const gp = group("Pots");
    gp.append(rowEl("lbl-row", span("lbl-key", "PAN group"), labelInput(CTRL.pots.groupLabel)));
    grid.append(gp);

    // Switches — two state labels each.
    const gsw = group("Switches");
    gsw.append(rowEl("lbl-row lbl-head", span("lbl-key", ""),
                     span("lbl-col", "state 0"), span("lbl-col", "state 1")));
    Object.entries(CTRL.switches).forEach(([id, arr]) => {
      if (id.startsWith("_")) return;
      gsw.append(rowEl("lbl-row", span("lbl-key", id), labelInput(arr[0]), labelInput(arr[1])));
    });
    grid.append(gsw);

    box.append(grid);
  }

  // ---- CV source cards (depth + Off/Osc/Env). The pitch card is wired to
  //      the emulator; the rest are design preview. ----
  function rangeCtl(role, text, min, max, step, value, valText) {
    const lab = document.createElement("label");
    lab.textContent = text;
    const inp = document.createElement("input");
    inp.type = "range";
    inp.min = min; inp.max = max; inp.step = step; inp.value = value;
    if (role) inp.dataset.role = role;
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

  // ms/s label for a normalized 0..1 time knob (matches the emulator's mapping).
  function secText(v) {
    const s = 0.001 * Math.pow(10000, v);
    return s < 1 ? `${Math.round(s * 1000)} ms` : `${s.toFixed(2)} s`;
  }
  const curveText = (v) => (v < -0.1 ? "log" : v > 0.1 ? "exp" : "lin");

  function makeCard({ title, note, target, wired }) {
    const card = rowEl("incard");
    card.dataset.source = "off";
    const top = rowEl("incard-top", span("incard-title", title));
    if (note) top.append(span("incard-target", note));
    card.append(top);
    card.append(rangeCtl("depth", "Depth", 0, 1, 0.01, 0.5));
    card.append(segCtl());

    const osc = rowEl("src src-osc", rangeCtl("freq", "Frequency", 0.02, 20, 0.01, 1, "1.00 Hz"));
    osc.hidden = true;
    const env = rowEl(
      "src src-env",
      rangeCtl("atk", "Attack", 0, 1, 0.001, 0.05, secText(0.05)),
      rangeCtl("dec", "Decay", 0, 1, 0.001, 0.4, secText(0.4)),
      rangeCtl("curve", "Curve", -1, 1, 0.01, 0, "lin")
    );
    env.hidden = true;
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
      freq: `${target}.cv.osc.freq`,
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
          if (inp.dataset.role === "atk" || inp.dataset.role === "dec") val.textContent = secText(v);
          else if (inp.dataset.role === "curve") val.textContent = curveText(v);
          else if (inp.dataset.role === "freq") val.textContent = `${v.toFixed(2)} Hz`;
        }
      });
    });
    const trig = card.querySelector(".trig");
    if (trig) trig.addEventListener("click", () => send(`${target}.cv.env.trig`, 1));
  }

  // Main CV panel: one card per param jack (+ GATE). Pitch is wired.
  function buildParamCards() {
    const box = document.getElementById("cv-cards");
    if (!box) return;
    box.innerHTML = "";
    Object.keys(CTRL.knobs).forEach((param) => {
      const jackT = CTRL.cvInputs.params[param];
      box.append(makeCard({
        title: jackT ? `${labelOf(jackT)} CV` : `${param} CV`,
        note: `→ ${labelOf(CTRL.knobs[param])}`,
        target: param,
        wired: param === "pitchHz",
      }));
    });
    // GATE jack (digital) — design placeholder.
    const gate = rowEl("incard is-empty",
      rowEl("incard-top", span("incard-title", labelOf(CTRL.cvInputs.gate)),
            span("incard-target", "→ sync/trig")),
      span("src-empty", "digital gate — TBD"));
    box.append(gate);
  }

  function buildSliderCvCards() {
    const box = document.getElementById("slider-cv-cards");
    if (!box) return;
    box.innerHTML = "";
    // Each slider's CV sums into that slider's value (clamped) — target is the
    // slider id ("ampN"), so it's wired to the same target the panel slider uses.
    Object.entries(CTRL.sliders).forEach(([id, t]) =>
      box.append(makeCard({ title: `${labelOf(t)} CV`, target: id, wired: true }))
    );
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

  async function init() {
    try {
      CTRL = await (await fetch("controls.json")).json();
      await fetchSvgLabels();
      buildEditor();
      buildParamCards();
      buildSliderCvCards();
      attachSegHandler();
      applyLabels();
      restoreState(); // reapply persisted control values (survives restarts)
    } catch (e) {
      console.error("labels.js: failed to load labels", e);
    }
  }
  init();
})();
