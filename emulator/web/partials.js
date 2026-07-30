// Fox Tail emulator — partial viewer.
//
// Draws where the partials actually are. Every number here comes from
// /partials, which the audio thread publishes straight out of the running
// FoxTailOsc — nothing about the layout is recomputed in JS, so the picture
// cannot drift from what you are hearing.
//
// X = frequency (log). Y = amplitude (dB). Colour = pan (blue left, red right).
// Shaded band = the shaper window. Dashed lines = the 9 octave-band edges.

(function () {
  const canvas = document.getElementById("partials-canvas");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");

  const F_MIN = 20;
  const F_MAX = 24000;
  const REF = 0.25; // amplitude that reads as full scale on the plot
  const FLOOR_DB = -72;

  const logMin = Math.log(F_MIN);
  const logSpan = Math.log(F_MAX) - logMin;
  const fx = (hz) => (Math.log(Math.max(hz, F_MIN)) - logMin) / logSpan;

  function ampToY(a) {
    if (a <= 0) return 0;
    const db = 20 * Math.log10(a / REF);
    return Math.max(0, Math.min(1, (db - FLOOR_DB) / -FLOOR_DB));
  }

  // Blue (hard left) -> grey (centre) -> red (hard right).
  function panColor(l, r, alpha) {
    const p = l + r > 1e-9 ? (r - l) / (r + l) : 0; // -1..1
    const t = (p + 1) / 2;
    const cr = Math.round(90 + t * 150);
    const cb = Math.round(240 - t * 150);
    const cg = Math.round(110 - Math.abs(p) * 40);
    return `rgba(${cr},${cg},${cb},${alpha})`;
  }

  let W = 0, H = 0;
  function resize() {
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    W = Math.max(1, Math.round(rect.width));
    H = Math.max(1, Math.round(rect.height));
    canvas.width = W * dpr;
    canvas.height = H * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
  window.addEventListener("resize", resize);

  function gridline(hz, label, style, dash) {
    const x = fx(hz) * W;
    if (x < 0 || x > W) return;
    ctx.save();
    ctx.strokeStyle = style;
    ctx.setLineDash(dash || []);
    ctx.beginPath();
    ctx.moveTo(x + 0.5, 0);
    ctx.lineTo(x + 0.5, H);
    ctx.stroke();
    ctx.restore();
    if (label) {
      ctx.fillStyle = "rgba(200,200,210,0.55)";
      ctx.font = "10px ui-monospace, monospace";
      ctx.fillText(label, x + 3, 11);
    }
  }

  function draw(frame) {
    ctx.clearRect(0, 0, W, H);

    // Shaper window, behind everything.
    if (frame.winEnd > frame.winStart) {
      const x0 = fx(frame.winStart) * W;
      const x1 = fx(frame.winEnd) * W;
      ctx.fillStyle = "rgba(216,98,58,0.10)";
      ctx.fillRect(x0, 0, Math.max(1, x1 - x0), H);
      ctx.strokeStyle = "rgba(216,98,58,0.45)";
      ctx.setLineDash([3, 3]);
      ctx.beginPath();
      ctx.moveTo(x0 + 0.5, 0); ctx.lineTo(x0 + 0.5, H);
      ctx.moveTo(x1 + 0.5, 0); ctx.lineTo(x1 + 0.5, H);
      ctx.stroke();
      ctx.setLineDash([]);
    }

    // Decade gridlines, then the octave-band edges (partial 1, 2, 4, ... 256).
    [100, 1000, 10000].forEach((hz) =>
      gridline(hz, hz >= 1000 ? hz / 1000 + "k" : String(hz), "rgba(255,255,255,0.07)")
    );
    // Band centres. Bands are geometric over the whole bank, so slider b sits at
    // ratio N^((b+0.5)/bands) — not at an octave unless bands == log2(N).
    if (frame.f0 > 0 && frame.bands > 0) {
      for (let b = 0; b < frame.bands; b++) {
        const ratio = Math.pow(frame.nPartials, (b + 0.5) / frame.bands);
        gridline(frame.f0 * ratio, "b" + (b + 1), "rgba(120,170,255,0.16)", [2, 4]);
      }
    }
    gridline(frame.nyquist, "Nyq", "rgba(255,80,80,0.35)", [4, 3]);

    // Baseline.
    ctx.strokeStyle = "rgba(255,255,255,0.12)";
    ctx.beginPath();
    ctx.moveTo(0, H - 0.5); ctx.lineTo(W, H - 0.5);
    ctx.stroke();

    // The partials themselves.
    const p = frame.partials;
    for (let i = 0; i < p.length; i += 3) {
      const hz = p[i], l = p[i + 1], r = p[i + 2];
      if (hz < F_MIN) continue;
      const x = fx(hz) * W;
      const a = Math.sqrt(l * l + r * r);
      if (a <= 1e-6) {
        // Muted (above Nyquist, or its band is down) — a faint tick, so the
        // Nyquist wall and the fade are visible rather than just absent.
        ctx.fillStyle = "rgba(255,255,255,0.06)";
        ctx.fillRect(x, H - 3, 1, 3);
        continue;
      }
      const y = ampToY(a) * (H - 6);
      ctx.fillStyle = panColor(l, r, 0.85);
      ctx.fillRect(x, H - y, 1, y);
    }

    // Count what is actually audible — the single most useful readout when the
    // Nyquist wall is eating your top bands.
    let live = 0;
    for (let i = 0; i < p.length; i += 3)
      if (Math.sqrt(p[i + 1] * p[i + 1] + p[i + 2] * p[i + 2]) > 1e-4) live++;
    const el = document.getElementById("partial-count");
    if (el) el.textContent = `${live} / ${p.length / 3} audible`;
  }

  function parse(txt) {
    const parts = txt.split(";");
    const f0 = parseFloat(parts[0]);
    const nyquist = parseFloat(parts[1]);
    const winStart = parseFloat(parts[2]);
    const winEnd = parseFloat(parts[3]);
    const nPartials = parseInt(parts[4], 10);
    const bands = parseInt(parts[5], 10);
    const partials = new Float32Array((parts.length - 6) * 3);
    let n = 0;
    for (let i = 6; i < parts.length; i++) {
      const t = parts[i].split(":");
      partials[n++] = parseFloat(t[0]);
      partials[n++] = parseFloat(t[1]);
      partials[n++] = parseFloat(t[2]);
    }
    return { f0, nyquist, winStart, winEnd, nPartials, bands, partials };
  }

  async function poll() {
    try {
      const txt = await (await fetch("/partials")).text();
      if (!W) resize();
      draw(parse(txt));
    } catch (_) {}
    setTimeout(poll, 50); // 20 Hz — enough to watch a Shepard glide
  }

  resize();
  poll();
})();