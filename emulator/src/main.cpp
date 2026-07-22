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

std::atomic<float> g_amp0{0.f};     // DRY slider -> harmonic 0 amplitude
std::atomic<float> g_pitchHz{220.f}; // TIME knob -> fundamental frequency
std::atomic<float> g_master{0.7f};   // host master volume

std::atomic<float> g_meter0{0.f}; // fed back to the UI for the LED

foxtail::FoxTailOsc g_osc;

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

    foxtail::Controls c;
    c.amp[0]  = g_amp0.load(std::memory_order_relaxed);
    c.pitchHz = g_pitchHz.load(std::memory_order_relaxed);
    c.master  = g_master.load(std::memory_order_relaxed);

    g_osc.Process(c, g_scratchL.data(), g_scratchR.data(), frameCount);

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        out[2 * i + 0] = g_scratchL[i];
        out[2 * i + 1] = g_scratchR[i];
    }

    g_meter0.store(g_osc.Meter(0), std::memory_order_relaxed);
}

// Map a control id coming from the UI to its atomic. Returns false if unknown.
bool setControl(const std::string& id, float value) {
    if (id == "amp0")    { g_amp0.store(value, std::memory_order_relaxed);    return true; }
    if (id == "pitchHz") { g_pitchHz.store(value, std::memory_order_relaxed); return true; }
    if (id == "master")  { g_master.store(value, std::memory_order_relaxed);  return true; }
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
