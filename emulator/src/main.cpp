//
// Fox Tail emulator — native audio + web UI host.
//
//   * Audio: miniaudio -> CoreAudio, runs the real foxtail::FoxTailOsc DSP.
//   * UI:    cpp-httplib serves emulator/web/* on localhost:4343; the browser
//            sends control changes as tiny GET hits and polls meters back.
//
// Control state is shared with the audio thread via lock-free atomics, so the
// audio callback never blocks. This is a dev tool, kept entirely separate from
// the firmware build.
//
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "httplib.h"

#include "foxtail_dsp.h" // the shared, hardware-free DSP (repo root)
#include "sources.h"     // emulator-only modulation sources (envelopes, ...)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Shared control state (HTTP thread writes, audio thread reads).
// ---------------------------------------------------------------------------
namespace {

constexpr int   kPort       = 4343;
constexpr float kSampleRate = 48000.f;

// Map a normalized 0..1 knob to a time in seconds (1 ms .. 10 s, log law).
static float normToSec(float v) { return 0.001f * std::pow(10000.f, v); }

constexpr float kPitchMaxOct = 4.f; // pitch-CV depth 1.0 -> +4 octaves

// A modulation source feeding one CV target. Params are set by the HTTP thread
// (atomics); the Envelope/Oscillator objects are advanced only by the audio
// thread. These stand in for external modules patched into the CV jacks — they
// exist ONLY in the emulator.
struct CvSource {
    std::atomic<int>   type{0};        // 0 off, 1 osc, 2 env
    std::atomic<float> depth{0.f};     // 0..1
    std::atomic<float> atk{0.05f};     // env attack  (normalized 0..1)
    std::atomic<float> dec{0.40f};     // env decay   (normalized 0..1)
    std::atomic<float> curve{0.f};     // env curve   (-1..1)
    std::atomic<int>   cycle{0};       // env: loop instead of one-shot
    std::atomic<int>   trig{0};        // set 1 to fire the envelope
    std::atomic<float> oscCoarse{0.f}; // osc coarse freq (0..20000 Hz)
    std::atomic<float> oscFine{1.f};   // osc fine freq   (0.02..20 Hz)
    ftemu::Envelope    env;            // audio-thread only
    ftemu::Oscillator  osc;            // audio-thread only

    // Advance one block; return the source signal scaled by depth.
    // (envelope is unipolar 0..1, oscillator bipolar -1..1.)
    float run(float dt) {
        const int t = type.load(std::memory_order_relaxed);
        float sig = 0.f;
        if (t == 2) {
            env.attack  = normToSec(atk.load(std::memory_order_relaxed));
            env.decay   = normToSec(dec.load(std::memory_order_relaxed));
            env.curve   = curve.load(std::memory_order_relaxed);
            env.cycling = cycle.load(std::memory_order_relaxed) != 0;
            if (env.cycling && env.stage == ftemu::Envelope::Idle) env.trigger();
            if (trig.exchange(0, std::memory_order_relaxed)) env.trigger();
            sig = env.process(dt);
        } else if (t == 1) {
            // Coarse + fine frequency, summed and clamped to the audio range.
            float f = oscCoarse.load(std::memory_order_relaxed)
                    + oscFine.load(std::memory_order_relaxed);
            osc.freq = f < 0.f ? 0.f : (f > 20000.f ? 20000.f : f);
            sig = osc.process(dt);
        }
        return sig * depth.load(std::memory_order_relaxed);
    }

    // Route a "cv.<leaf>" parameter. Returns false if the leaf is unknown.
    bool set(const std::string& leaf, float v) {
        if (leaf == "cv.src")         { type.store((int)v, std::memory_order_relaxed);   return true; }
        if (leaf == "cv.depth")       { depth.store(v, std::memory_order_relaxed);       return true; }
        if (leaf == "cv.env.attack")  { atk.store(v, std::memory_order_relaxed);         return true; }
        if (leaf == "cv.env.decay")   { dec.store(v, std::memory_order_relaxed);         return true; }
        if (leaf == "cv.env.curve")   { curve.store(v, std::memory_order_relaxed);       return true; }
        if (leaf == "cv.env.cycle")   { cycle.store((int)v, std::memory_order_relaxed);  return true; }
        if (leaf == "cv.env.trig")    { trig.store(1, std::memory_order_relaxed);        return true; }
        if (leaf == "cv.osc.coarse")  { oscCoarse.store(v, std::memory_order_relaxed);   return true; }
        if (leaf == "cv.osc.fine")    { oscFine.store(v, std::memory_order_relaxed);     return true; }
        return false;
    }
};

// Panel knobs 2..5, numbered left to right (knob 1 is pitch and is handled
// separately). Knobs 2 and 3 mean the same thing in both shaper modes; 4 and 5
// change meaning with switch 1.
enum ShaperKnob {
    kKnob2 = 0,      // window start
    kKnob3,          // window width
    kKnob4,          // cluster: partials per cluster | shepard: phi
    kKnob5,          // shapeB: cluster density       | shepard: window gain
    kNumShaperKnobs,
};

// Physical sliders/pots, 9 of each, exactly as on the panel. Slider 1 and pot 1
// are inharmonicity and master level; sliders/pots 2..9 are the 8 bands. The
// mapping happens in the audio callback so the UI ids stay physical.
constexpr int kNumSliders = foxtail::kNumBands + 1; // 9
constexpr int kBandOffset = 1;

std::atomic<float> g_slider[kNumSliders];       // raw slider values 0..1
std::atomic<float> g_pot[kNumSliders];          // raw pot values, -1..1
std::atomic<float> g_knob[kNumShaperKnobs];     // the four shaper knobs, 0..1
std::atomic<float> g_pitchHz{220.f};            // knob 1 -> fundamental
std::atomic<float> g_master{0.7f};              // host master volume
std::atomic<int>   g_mode{foxtail::kModeCluster}; // switch 1: cluster/shepard
std::atomic<int>   g_parity{0};                   // switch 2: 0 = all, 1 = odd

CvSource g_srcSlider[kNumSliders];              // per-slider CV source (jack each)
CvSource g_srcKnob[kNumShaperKnobs];            // per-knob CV source (analog jacks)
CvSource g_srcPitch;                            // pitch CV source

std::atomic<float> g_meter[kNumSliders];        // LED feedback: [0]=inharm, then bands
std::atomic<float> g_pitchCvDbg{0.f};           // last pitch CV (octaves), /dbg

// --- Partial-viewer snapshot ------------------------------------------------
// The audio thread publishes the engine's actual per-partial state; the HTTP
// thread reads it with a seqlock (bump the counter to odd while writing, back to
// even when stable; a reader that sees an odd or changed counter retries). This
// is emulator-only introspection — nothing here runs on the module.
std::atomic<unsigned> g_snapSeq{0};
float g_snapFreq[foxtail::kNumPartials];
float g_snapL[foxtail::kNumPartials];
float g_snapR[foxtail::kNumPartials];
std::atomic<float> g_snapWinStartHz{0.f};
std::atomic<float> g_snapWinEndHz{0.f};
std::atomic<float> g_snapF0{0.f};

foxtail::FoxTailOsc g_osc;

static bool startsWith(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

// Reusable per-channel scratch so we don't allocate in the audio callback.
std::vector<float> g_scratchL;
std::vector<float> g_scratchR;

void audioCallback(ma_device* /*device*/, void* pOutput, const void* /*pInput*/,
                   ma_uint32 frameCount) {
    float* out = static_cast<float*>(pOutput); // interleaved stereo f32

    if (g_scratchL.size() < frameCount) {
        g_scratchL.resize(frameCount);
        g_scratchR.resize(frameCount);
    }

    // Run in fixed 5-frame chunks — the firmware's block size. Controls and CV
    // sources are therefore updated at exactly the rate the module updates them
    // (9.6 kHz), not at whatever buffer size CoreAudio happens to hand us.
    // Before this, a 512-frame buffer sampled the CV sources at 94 Hz, so any
    // modulator above ~47 Hz aliased down to something far slower than its label
    // claimed — that is not FM, it is a staircase. The module has the same
    // ceiling (~4.8 kHz); the emulator should show it, not hide it.
    constexpr ma_uint32 kEmuBlock = 5;

    for (ma_uint32 off = 0; off < frameCount; off += kEmuBlock) {
        const ma_uint32 n  = (frameCount - off) < kEmuBlock ? (frameCount - off) : kEmuBlock;
        const float     dt = (float)n / kSampleRate;

        foxtail::Controls c;

        // Slider 1 -> inharmonicity, pot 1 -> master level.
        {
            float v = g_slider[0].load(std::memory_order_relaxed) + g_srcSlider[0].run(dt);
            c.inharm = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        }
        // Pot 1 -> inharmonicity onset (partial below which the series stays
        // harmonic). Master is host-side only; the module uses a fixed level.
        c.inharmOnset = 0.5f * (g_pot[0].load(std::memory_order_relaxed) + 1.f);
        c.master      = g_master.load(std::memory_order_relaxed);

        // Sliders/pots 2..9 -> the 8 bands.
        for (int b = 0; b < foxtail::kNumBands; ++b) {
            const int s = b + kBandOffset;
            float g = g_slider[s].load(std::memory_order_relaxed) + g_srcSlider[s].run(dt);
            c.bandGain[b] = g < 0.f ? 0.f : (g > 1.f ? 1.f : g);
            // Pots are centre-detented -1..1 on the panel; the DSP wants 0..1.
            c.bandPan[b] = 0.5f * (g_pot[s].load(std::memory_order_relaxed) + 1.f);
        }

        // The four shaper knobs, each summed with its analog CV jack.
        float k[kNumShaperKnobs];
        for (int i = 0; i < kNumShaperKnobs; ++i) {
            float v = g_knob[i].load(std::memory_order_relaxed) + g_srcKnob[i].run(dt);
            k[i] = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        }
        c.position = k[kKnob2];
        c.window   = k[kKnob3];
        c.shapeA   = k[kKnob4];
        c.shapeB   = k[kKnob5];

        c.mode        = g_mode.load(std::memory_order_relaxed);
        c.parity      = g_parity.load(std::memory_order_relaxed) ? 1.f : 0.f;
        c.pitchHz     = g_pitchHz.load(std::memory_order_relaxed);
        c.pitchCv     = g_srcPitch.run(dt) * kPitchMaxOct; // depth already applied
        g_pitchCvDbg.store(c.pitchCv, std::memory_order_relaxed);

        g_osc.Process(c, g_scratchL.data() + off, g_scratchR.data() + off, n);
    }

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        out[2 * i + 0] = g_scratchL[i];
        out[2 * i + 1] = g_scratchR[i];
    }

    g_meter[0].store(g_slider[0].load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (int b = 0; b < foxtail::kNumBands; ++b)
        g_meter[b + kBandOffset].store(g_osc.Meter(b), std::memory_order_relaxed);

    // Publish the partial snapshot for the viewer.
    // Controls are per-chunk now, so recover the last chunk's f0 from the atomics.
    const float f0 = g_pitchHz.load(std::memory_order_relaxed)
                   * std::pow(2.f, g_pitchCvDbg.load(std::memory_order_relaxed));
    g_snapSeq.fetch_add(1, std::memory_order_release); // -> odd, writing
    for (int k = 0; k < foxtail::kNumPartials; ++k) {
        g_snapFreq[k] = g_osc.PartialFreq(k);
        g_snapL[k]    = g_osc.PartialAmpL(k);
        g_snapR[k]    = g_osc.PartialAmpR(k);
    }
    g_snapF0.store(f0, std::memory_order_relaxed);
    g_snapWinStartHz.store(g_osc.WindowStart() * f0, std::memory_order_relaxed);
    g_snapWinEndHz.store(g_osc.WindowEnd() * f0, std::memory_order_relaxed);
    g_snapSeq.fetch_add(1, std::memory_order_release); // -> even, stable
}

// Panel knob id -> shaper knob index. Ids match controls.json so the label
// editor keeps working; what they *do* is set by the mode switch.
int shaperKnobIndex(const std::string& name) {
    if (name == "knob2") return kKnob2;
    if (name == "knob3") return kKnob3;
    if (name == "knob4")      return kKnob4;
    if (name == "knob5")      return kKnob5;
    return -1;
}

// Map a UI control id to state. Ids are either a base value ("amp3", "pan3",
// "knob2", "knob1", "master") or a per-target CV param
// ("amp3.cv.env.attack", "knob2.cv.src", "knob1.cv.src").
bool setControl(const std::string& id, float value) {
    if (id == "master")    { g_master.store(value, std::memory_order_relaxed);  return true; }
    if (id == "knob1")   { g_pitchHz.store(value, std::memory_order_relaxed); return true; }
    if (id == "switch1")    { g_mode.store((int)value, std::memory_order_relaxed); return true; }
    if (id == "switch2") { g_parity.store((int)value, std::memory_order_relaxed); return true; }

    // Pitch CV: "knob1.cv.*"  (strip the "knob1." prefix -> "cv.<leaf>")
    if (startsWith(id, "knob1.cv."))
        return g_srcPitch.set(id.substr(6), value);

    const size_t dot = id.find('.');
    const std::string head = (dot == std::string::npos) ? id : id.substr(0, dot);

    // Shaper knobs: "knob2" / "knob5" / ... or "<knob>.cv.*"
    const int ki = shaperKnobIndex(head);
    if (ki >= 0) {
        if (dot == std::string::npos) { g_knob[ki].store(value, std::memory_order_relaxed); return true; }
        return g_srcKnob[ki].set(id.substr(dot + 1), value);
    }

    // Sliders: "ampN" or "ampN.cv.*"
    if (startsWith(head, "amp")) {
        int idx = std::atoi(head.c_str() + 3); // "amp3" -> 3
        if (idx < 0 || idx >= kNumSliders) return false;
        if (dot == std::string::npos) { g_slider[idx].store(value, std::memory_order_relaxed); return true; }
        return g_srcSlider[idx].set(id.substr(dot + 1), value); // "cv.<leaf>"
    }

    // Pots: "panN" -> per-band pan. No CV jack on the hardware, so no cv route.
    if (startsWith(head, "pan") && dot == std::string::npos) {
        int idx = std::atoi(head.c_str() + 3);
        if (idx < 0 || idx >= kNumSliders) return false;
        g_pot[idx].store(value, std::memory_order_relaxed);
        return true;
    }
    return false;
}

// --- Panel label editing --------------------------------------------------
// The panel SVG is the single source of truth for label text. We read labels
// from it (GET /svg-labels) and rename by rewriting one <tspan> matched by its
// id, then re-rendering the PNG (POST /label). Paths are relative to WEB_DIR
// (=.../emulator/web), so they hold regardless of the process CWD.
std::string svgPath()      { return std::string(WEB_DIR) + "/../../panel/Fox-Tail.svg"; }
std::string renderScript() { return std::string(WEB_DIR) + "/../render_panel.py"; }

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << data;
    return f.good();
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}
std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

// Build {"<tspanId>":"<text>", ...} for every <tspan id="...">text</tspan>.
std::string svgLabelsJson() {
    const std::string svg = readFile(svgPath());
    std::string out = "{";
    bool first = true;
    size_t pos = 0;
    while ((pos = svg.find("<tspan", pos)) != std::string::npos) {
        size_t tagEnd = svg.find('>', pos);
        if (tagEnd == std::string::npos) break;
        const std::string tag = svg.substr(pos, tagEnd - pos);
        size_t idp = tag.find("id=\"");
        if (idp != std::string::npos) {
            size_t ids = idp + 4;
            size_t ide = tag.find('"', ids);
            const std::string id = tag.substr(ids, ide - ids);
            size_t textStart = tagEnd + 1;
            size_t textEnd = svg.find('<', textStart);
            const std::string text = trim(svg.substr(textStart, textEnd - textStart));
            if (!text.empty()) {
                if (!first) out += ",";
                first = false;
                out += "\"" + jsonEscape(id) + "\":\"" + jsonEscape(text) + "\"";
            }
        }
        pos = tagEnd + 1;
    }
    out += "}";
    return out;
}

// Replace the text of one <tspan> (matched by id) and re-render the PNG.
// Returns "ok", or an error string.
std::string renameLabel(const std::string& tspan, const std::string& text) {
    std::string svg = readFile(svgPath());
    if (svg.empty()) return "svg-read-failed";
    // The trailing quote in the anchor prevents prefix collisions
    // (id="tspan5244" must not match id="tspan5244-8").
    const std::string anchor = "id=\"" + tspan + "\"";
    size_t idp = svg.find(anchor);
    if (idp == std::string::npos) return "tspan-not-found";
    size_t gt = svg.find('>', idp);
    if (gt == std::string::npos) return "malformed";
    size_t lt = svg.find('<', gt);
    if (lt == std::string::npos) return "malformed";
    svg = svg.substr(0, gt + 1) + xmlEscape(text) + svg.substr(lt);
    if (!writeFile(svgPath(), svg)) return "svg-write-failed";
    const std::string cmd = "python3 '" + renderScript() + "' >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0 ? "ok" : "render-failed";
}

} // namespace

int main() {
    // --- Audio ---
    g_osc.Init(kSampleRate);

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = static_cast<ma_uint32>(kSampleRate);
    cfg.dataCallback      = audioCallback;

    ma_device device;
    if (ma_device_init(nullptr, &cfg, &device) != MA_SUCCESS) {
        std::fprintf(stderr, "Fox Tail: failed to open audio device\n");
        return 1;
    }
    if (ma_device_start(&device) != MA_SUCCESS) {
        std::fprintf(stderr, "Fox Tail: failed to start audio device\n");
        ma_device_uninit(&device);
        return 1;
    }
    std::printf("Fox Tail emulator: audio running (%s, %d Hz)\n",
                ma_get_backend_name(device.pContext->backend),
                (int)device.sampleRate);

    // --- Web server ---
    httplib::Server srv;

    // Static UI. WEB_DIR is passed in by CMake so the binary finds the assets.
    const std::string webDir = WEB_DIR;
    srv.set_mount_point("/", webDir);

    // Control change: /ctl?id=amp0&v=0.5
    srv.Get("/ctl", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.get_param_value("id");
        std::string v  = req.get_param_value("v");
        bool ok = false;
        if (!id.empty() && !v.empty()) ok = setControl(id, std::stof(v));
        res.set_content(ok ? "ok" : "unknown", "text/plain");
    });

    // Meter poll: comma-separated per-band levels, one per LED.
    srv.Get("/meters", [](const httplib::Request&, httplib::Response& res) {
        std::string out;
        char buf[32];
        for (int b = 0; b < kNumSliders; ++b) {
            std::snprintf(buf, sizeof(buf), "%.4f", g_meter[b].load(std::memory_order_relaxed));
            if (b) out += ",";
            out += buf;
        }
        res.set_content(out, "text/plain");
    });

    // Partial viewer feed. Header line "f0;nyquist;winStartHz;winEndHz", then one
    // "freq:ampL:ampR" triple per partial, semicolon separated. Read under the
    // seqlock so a half-written frame is retried rather than drawn.
    srv.Get("/partials", [](const httplib::Request&, httplib::Response& res) {
        static thread_local std::vector<float> f(foxtail::kNumPartials),
            l(foxtail::kNumPartials), r(foxtail::kNumPartials);
        float f0 = 0.f, ws = 0.f, we = 0.f;
        for (int tries = 0; tries < 8; ++tries) {
            const unsigned s0 = g_snapSeq.load(std::memory_order_acquire);
            if (s0 & 1u) continue; // mid-write
            std::memcpy(f.data(), g_snapFreq, sizeof(g_snapFreq));
            std::memcpy(l.data(), g_snapL, sizeof(g_snapL));
            std::memcpy(r.data(), g_snapR, sizeof(g_snapR));
            f0 = g_snapF0.load(std::memory_order_relaxed);
            ws = g_snapWinStartHz.load(std::memory_order_relaxed);
            we = g_snapWinEndHz.load(std::memory_order_relaxed);
            if (g_snapSeq.load(std::memory_order_acquire) == s0) break; // stable
        }
        std::string out;
        out.reserve(16 * foxtail::kNumPartials);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.3f;%.1f;%.3f;%.3f;%d;%d", f0, kSampleRate * 0.5f,
                      ws, we, foxtail::kNumPartials, foxtail::kNumBands);
        out += buf;
        for (int k = 0; k < foxtail::kNumPartials; ++k) {
            std::snprintf(buf, sizeof(buf), ";%.2f:%.5f:%.5f", f[k], l[k], r[k]);
            out += buf;
        }
        res.set_content(out, "text/plain");
    });

    // Debug: current pitch-CV value in octaves (for verifying modulation).
    srv.Get("/dbg", [](const httplib::Request&, httplib::Response& res) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "pitchCv=%.4f", g_pitchCvDbg.load(std::memory_order_relaxed));
        res.set_content(buf, "text/plain");
    });

    // The control->tspan mapping lives in emulator/controls.json (one level up
    // from the served web dir); expose it to the client.
    srv.Get("/controls.json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(readFile(std::string(WEB_DIR) + "/../controls.json"), "application/json");
    });

    // Current label text of every SVG tspan (the label source of truth).
    srv.Get("/svg-labels", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(svgLabelsJson(), "application/json");
    });

    // Rename one label: /label?tspan=tspan2655&text=NEWNAME  (rewrites the SVG
    // + re-renders the PNG). The client knows tspan ids from controls.json.
    srv.Post("/label", [](const httplib::Request& req, httplib::Response& res) {
        const std::string tspan = req.get_param_value("tspan");
        const std::string text  = req.get_param_value("text");
        if (tspan.empty()) { res.status = 400; res.set_content("no tspan", "text/plain"); return; }
        const std::string r = renameLabel(tspan, text);
        if (r != "ok") res.status = 500;
        res.set_content(r, "text/plain");
    });

    std::printf("Fox Tail emulator: UI at http://localhost:%d\n", kPort);
    srv.listen("localhost", kPort); // blocks

    ma_device_uninit(&device);
    return 0;
}
