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

  // ---- Slider-CV panel (design preview: depth + Off/Osc/Env source) ----
  function rangeCtl(text, min, max, step, value, valText) {
    const lab = document.createElement("label");
    lab.textContent = text;
    const inp = document.createElement("input");
    inp.type = "range";
    inp.min = min; inp.max = max; inp.step = step; inp.value = value;
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
  function sliderCvCard(title) {
    const card = rowEl("incard");
    card.dataset.source = "off";
    card.append(rowEl("incard-top", span("incard-title", title)));
    card.append(rangeCtl("Depth", 0, 1, 0.01, 0.5));
    card.append(segCtl());

    const osc = rowEl("src src-osc", rangeCtl("Frequency", 0.02, 20, 0.01, 1, "1.00 Hz"));
    osc.hidden = true;
    const env = rowEl(
      "src src-env",
      rangeCtl("Attack", 0, 1, 0.001, 0.1, "10 ms"),
      rangeCtl("Decay", 0, 1, 0.001, 0.4, "200 ms"),
      rangeCtl("Curve", -1, 1, 0.01, 0, "lin")
    );
    env.hidden = true;
    const trig = document.createElement("button");
    trig.className = "trig";
    trig.textContent = "Trigger";
    env.append(trig);
    card.append(osc, env);
    return card;
  }
  function buildSliderCvCards() {
    const box = document.getElementById("slider-cv-cards");
    if (!box) return;
    box.innerHTML = "";
    Object.values(CTRL.sliders).forEach((t) =>
      box.append(sliderCvCard(`${labelOf(t)} CV`))
    );
  }
  // One delegated handler drives every Off/Osc/Env picker (both input panels).
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
    });
  }

  async function init() {
    try {
      CTRL = await (await fetch("controls.json")).json();
      await fetchSvgLabels();
      buildEditor();
      buildSliderCvCards();
      attachSegHandler();
      applyLabels();
    } catch (e) {
      console.error("labels.js: failed to load labels", e);
    }
  }
  init();
})();
