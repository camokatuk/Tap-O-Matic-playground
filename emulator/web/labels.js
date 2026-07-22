// labels.js — the panel SVG owns all label text; this ties the emulator UI to
// it and lets you rename labels live. Loads controls.json (control id -> tspan
// id) + GET /svg-labels (tspan id -> text), applies them across the UI, and
// hosts the label editor. Renaming POSTs /label (rewrites the tspan + re-renders
// the PNG). The CV source cards live in sources.js; this file just orchestrates
// the load and exposes the model as FT.model. Uses shared helpers from app.js
// (span, rowEl, restoreState). Loaded after app.js and sources.js.
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

    if (FT.sources) FT.sources.refreshTitles(); // CV card titles track labels
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

  // ---- label editor ----
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

  async function init() {
    try {
      CTRL = await (await fetch("controls.json")).json();
      await fetchSvgLabels();
      FT.model = { ctrl: CTRL, labelOf };  // expose model to sources.js
      buildEditor();
      FT.sources.buildParamCards();
      FT.sources.buildSliderCvCards();
      FT.sources.attachSegHandler();
      applyLabels();
      restoreState(); // reapply persisted control values (survives restarts)
    } catch (e) {
      console.error("labels.js: failed to load", e);
    }
  }
  init();
})();
