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

std::atomic<float> g_amp[foxtail::kNumHarmonics]; // per-harmonic slider base values
std::atomic<float> g_pitchHz{220.f};              // TIME knob -> fundamental
std::atomic<float> g_master{0.7f};                // host master volume

CvSource g_srcAmp[foxtail::kNumHarmonics];        // per-slider CV source
CvSource g_srcPitch;                              // pitch CV source

std::atomic<float> g_meter0{0.f};                 // LED feedback for the UI
std::atomic<float> g_pitchCvDbg{0.f};             // last pitch CV (octaves), /dbg

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

    const float dt = (float)frameCount / kSampleRate;

    foxtail::Controls c;
    // Each slider: effective amplitude = clamp(base + its CV, 0..1). The CV
    // routing is this single line per slider, so decoupling a slider from its
    // CV later is trivial (drop/redirect the g_srcAmp[h] term).
    for (int h = 0; h < foxtail::kNumHarmonics; ++h) {
        float base = g_amp[h].load(std::memory_order_relaxed);
        float amp  = base + g_srcAmp[h].run(dt);
        c.amp[h]   = amp < 0.f ? 0.f : (amp > 1.f ? 1.f : amp);
    }

    c.pitchHz = g_pitchHz.load(std::memory_order_relaxed);
    c.pitchCv = g_srcPitch.run(dt) * kPitchMaxOct; // depth already applied
    c.master  = g_master.load(std::memory_order_relaxed);
    g_pitchCvDbg.store(c.pitchCv, std::memory_order_relaxed);

    g_osc.Process(c, g_scratchL.data(), g_scratchR.data(), frameCount);

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        out[2 * i + 0] = g_scratchL[i];
        out[2 * i + 1] = g_scratchR[i];
    }

    g_meter0.store(g_osc.Meter(0), std::memory_order_relaxed);
}

// Map a UI control id to state. Ids are either a base value ("amp3", "pitchHz",
// "master") or a per-target CV param ("amp3.cv.env.attack", "pitchHz.cv.src").
bool setControl(const std::string& id, float value) {
    if (id == "master")  { g_master.store(value, std::memory_order_relaxed);  return true; }
    if (id == "pitchHz") { g_pitchHz.store(value, std::memory_order_relaxed); return true; }

    // Pitch CV: "pitchHz.cv.*"  (strip the "pitchHz." prefix -> "cv.<leaf>")
    if (startsWith(id, "pitchHz.cv."))
        return g_srcPitch.set(id.substr(8), value);

    // Sliders: "ampN" or "ampN.cv.*"
    if (startsWith(id, "amp")) {
        size_t dot = id.find('.');
        std::string head = (dot == std::string::npos) ? id : id.substr(0, dot);
        int idx = std::atoi(head.c_str() + 3); // "amp3" -> 3
        if (idx < 0 || idx >= foxtail::kNumHarmonics) return false; // slider w/o harmonic
        if (dot == std::string::npos) { g_amp[idx].store(value, std::memory_order_relaxed); return true; }
        return g_srcAmp[idx].set(id.substr(dot + 1), value); // "cv.<leaf>"
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

    // Meter poll: returns comma-separated meter values for the LEDs.
    srv.Get("/meters", [](const httplib::Request&, httplib::Response& res) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.4f", g_meter0.load(std::memory_order_relaxed));
        res.set_content(buf, "text/plain");
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
