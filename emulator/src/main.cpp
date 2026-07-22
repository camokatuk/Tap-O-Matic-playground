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

    std::printf("Fox Tail emulator: UI at http://localhost:%d\n", kPort);
    srv.listen("localhost", kPort); // blocks

    ma_device_uninit(&device);
    return 0;
}
