// Fox Tail emulator — UI glue.
//
// Sends control changes to the native audio app as tiny GET hits
// (/ctl?id=..&v=..) and polls meters back. No audio here — the browser only
// carries control values; the C++ app does all the sound on CoreAudio.

const statusbar = document.getElementById("statusbar");

// Throttle outgoing control hits to animation frames so a fast drag doesn't
// flood the server (localhost is fast, but this keeps it tidy).
const pending = new Map();
let rafScheduled = false;
function send(id, value) {
  pending.set(id, value);
  if (!rafScheduled) {
    rafScheduled = true;
    requestAnimationFrame(flush);
  }
}
function flush() {
  rafScheduled = false;
  for (const [id, value] of pending) {
    fetch(`/ctl?id=${encodeURIComponent(id)}&v=${encodeURIComponent(value)}`);
  }
  pending.clear();
}

function status(text) {
  statusbar.textContent = text;
}
function clearStatus() {
  statusbar.innerHTML = "&nbsp;";
}

// Log <-> linear helpers for controls flagged data-log.
function logMap(t, min, max) {
  // t in 0..1 -> value in [min,max] on a log scale
  return min * Math.pow(max / min, t);
}
function logUnmap(v, min, max) {
  return Math.log(v / min) / Math.log(max / min);
}

// ---- "Unassigned" side-panel mirror -----------------------------------------
// Auto-list every [data-unassigned] control with a live value so its state is
// visible even though it isn't wired to the DSP yet.
const unassignedBox = document.getElementById("unassigned");
function reflectMirror(el, text) {
  const m = document.getElementById("mirror-" + el.dataset.id);
  if (m) m.textContent = text;
}
if (unassignedBox) {
  document.querySelectorAll("[data-unassigned]").forEach((el) => {
    const row = document.createElement("div");
    row.className = "uctl";
    row.innerHTML =
      '<span class="uctl-label"></span>' +
      '<span class="uctl-val" id="mirror-' + el.dataset.id + '"></span>';
    row.querySelector(".uctl-label").textContent = el.dataset.label || el.dataset.id;
    unassignedBox.appendChild(row);
  });
}

// ---- Plain range inputs (sliders) — only wired ones (they carry data-id) ----
document.querySelectorAll('input[type="range"][data-id]').forEach((el) => {
  const id = el.dataset.id;
  const label = el.dataset.label || id;

  const emit = () => {
    const v = parseFloat(el.value);
    send(id, v);
    status(`${label}: ${fmt(v)}`);
    if (id === "pitchHz") mirrorFreq(v);
  };
  el.addEventListener("input", emit);
  el.addEventListener("mouseenter", () =>
    status(`${label}: ${fmt(parseFloat(el.value))}`)
  );
  el.addEventListener("mouseleave", clearStatus);
});

function fmt(v) {
  return Math.abs(v) >= 100 ? v.toFixed(0) : v.toFixed(3);
}

// Keep the panel TIME knob and the host "Fundamental" slider in agreement.
const freqReadout = document.getElementById("freq-readout");
function mirrorFreq(hz) {
  if (freqReadout) freqReadout.textContent = hz.toFixed(0);
  document
    .querySelectorAll('[data-id="pitchHz"][data-mirror]')
    .forEach((m) => (m.value = hz));
  const knob = document.querySelector('.knob[data-id="pitchHz"]');
  if (knob) setKnobValue(knob, hz, /*silent=*/ true);
}

// ---- Knobs (drag up/down) ---------------------------------------------------
document.querySelectorAll(".knob, .tiny-pot").forEach((knob) => {
  const id = knob.dataset.id;
  const label = knob.dataset.label || id;
  const min = parseFloat(knob.dataset.min);
  const max = parseFloat(knob.dataset.max);
  const isLog = knob.dataset.log === "1";

  let value = parseFloat(knob.dataset.value);
  knob._toT = (v) => (isLog ? logUnmap(v, min, max) : (v - min) / (max - min));
  knob._fromT = (t) => (isLog ? logMap(t, min, max) : min + t * (max - min));
  knob._set = (v, silent) => setKnobValue(knob, v, silent);

  setKnobValue(knob, value, true);

  let dragging = false, lastY = 0, t = knob._toT(value);

  knob.addEventListener("mousedown", (e) => {
    dragging = true;
    lastY = e.clientY;
    t = knob._toT(parseFloat(knob.dataset.value));
    e.preventDefault();
  });
  window.addEventListener("mousemove", (e) => {
    if (!dragging) return;
    t = Math.min(1, Math.max(0, t + (lastY - e.clientY) / 200)); // 200px = full sweep
    lastY = e.clientY;
    const v = knob._fromT(t);
    setKnobValue(knob, v, false);
    if (id === "pitchHz") mirrorFreq(v);
  });
  window.addEventListener("mouseup", () => (dragging = false));

  knob.addEventListener("mouseenter", () =>
    status(`${label}: ${fmt(parseFloat(knob.dataset.value))}`)
  );
  knob.addEventListener("mouseleave", () => {
    if (!dragging) clearStatus();
  });
});

function setKnobValue(knob, v, silent) {
  const min = parseFloat(knob.dataset.min);
  const max = parseFloat(knob.dataset.max);
  v = Math.min(max, Math.max(min, v));
  knob.dataset.value = v;
  const t = knob._toT ? knob._toT(v) : (v - min) / (max - min);
  knob.style.setProperty("--angle", `${-135 + t * 270}deg`);
  reflectMirror(knob, fmt(v));
  if (!silent) {
    if (!knob.dataset.unassigned) send(knob.dataset.id, v); // unwired -> UI only
    status(`${knob.dataset.label || knob.dataset.id}: ${fmt(v)}`);
  }
}

// ---- Switches (two-position, click to toggle) -------------------------------
document.querySelectorAll(".switch").forEach((sw) => {
  const states = (sw.dataset.states || "0,1").split(",");
  const label = sw.dataset.label || sw.dataset.id;
  const apply = (v, silent) => {
    v = v ? 1 : 0;
    sw.dataset.value = v;
    sw.style.setProperty("--pos", v);
    reflectMirror(sw, states[v]);
    if (!silent) {
      if (!sw.dataset.unassigned) send(sw.dataset.id, v);
      status(`${label}: ${states[v]}`);
    }
  };
  apply(parseInt(sw.dataset.value) || 0, true);
  sw.addEventListener("click", () => apply(1 - (parseInt(sw.dataset.value) || 0), false));
  sw.addEventListener("mouseenter", () =>
    status(`${label}: ${states[parseInt(sw.dataset.value) || 0]}`)
  );
  sw.addEventListener("mouseleave", clearStatus);
});

// ---- Meter polling (LED) ----------------------------------------------------
const led0 = document.getElementById("led0");
async function pollMeters() {
  try {
    const txt = await (await fetch("/meters")).text();
    const m = parseFloat(txt);
    if (led0 && !Number.isNaN(m)) {
      const b = Math.min(1, m);
      led0.style.background = `rgb(${60 + b * 195},${b * 40},${b * 30})`;
      led0.style.boxShadow = `0 0 ${b * 10}px rgba(216,98,58,${b})`;
    }
  } catch (_) {}
  setTimeout(pollMeters, 33); // ~30 Hz
}
pollMeters();
