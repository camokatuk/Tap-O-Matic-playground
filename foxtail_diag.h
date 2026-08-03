// Fox Tail serial diagnostics. Firmware-only; the engine and emulator never
// see this file. With FOXTAIL_SERIAL_LOG off every method is an empty inline,
// so the callback keeps the exact instruction sequence of a listening build.
//
// What the status line means, and why each counter exists:
//   peak  = worst single block %, this window (CpuLoadMeter's max is a
//           since-boot latch, so it is Reset() every window).
//   late  = blocks that started >1.5 slots after the previous one. CpuLoadMeter
//           times the inside of a block that ran; it cannot see one that
//           started late, which is what starvation looks like from audio.
//   gap   = worst block-start interval, in slots; 1.00 is nominal.
//   loops = main-loop turns; LEDs and USB live there, so this collapsing is
//           starvation even while audio survives.
//   out   = loudest sample / quietest ~13 ms chunk. The chunk min resolves
//           dropouts far shorter than the 1 Hz log; 13 ms is long enough that
//           a healthy low-f0 patch never reads low off a zero crossing.
//   clip  = blocks whose peak reached the soft-clip knee.
//   d     = largest sample-to-sample jump. A click is a step; this sees one at
//           any event duration. FM (e.g. V/oct jitter) never moves it.
#pragma once

#if FOXTAIL_SERIAL_LOG

#include "daisy_patch_sm.h"
#include "util/CpuLoadMeter.h"
#include "foxtail_dsp.h"

#include <cmath>

namespace foxdiag {

class Diag
{
  public:
    void Init(float sampleRate, int blockSize)
    {
        meter_.Init(sampleRate, blockSize);
        lateTicks_ = (uint32_t)((float)daisy::System::GetTickFreq()
                                * ((float)blockSize / sampleRate) * 1.5f);
    }

    // Top of the audio callback.
    void BlockStart()
    {
        meter_.OnBlockStart();
        const uint32_t t = daisy::System::GetTick();
        if (lastTick_)
        {
            const uint32_t gap = t - lastTick_;
            if (gap > worstGap_) worstGap_ = gap;
            if (gap > lateTicks_) ++lateBlocks_;
        }
        lastTick_ = t;
    }

    // Bottom of the audio callback, after the engine has filled l/r.
    void BlockEnd(const float* l, const float* r, size_t size)
    {
        float blk = 0.f;
        for (size_t i = 0; i < size; ++i)
        {
            const float dl = fabsf(l[i] - prevL_);
            const float dr = fabsf(r[i] - prevR_);
            if (dl > maxStep_) maxStep_ = dl;
            if (dr > maxStep_) maxStep_ = dr;
            prevL_ = l[i];
            prevR_ = r[i];
            float a = fabsf(l[i]);
            const float b = fabsf(r[i]);
            if (b > a) a = b;
            if (a > blk) blk = a;
        }
        if (blk > outPeak_) outPeak_ = blk;
        if (blk >= foxtail::kClipKnee) ++clipBlocks_;
        if (blk > chunk_) chunk_ = blk;
        if (++chunkBlocks_ >= kChunkBlocks)
        {
            if (chunk_ < chunkMin_) chunkMin_ = chunk_;
            chunk_       = 0.f;
            chunkBlocks_ = 0;
        }
        meter_.OnBlockEnd();
    }

    void MainLoopTick() { ++mainLoops_; }

    // One status line per window; resets the window counters.
    template <typename Hw>
    void PrintStatus(Hw& hw)
    {
        const float slots = lateTicks_ > 0
                                ? (float)worstGap_ / ((float)lateTicks_ / 1.5f)
                                : 0.f;
        hw.PrintLine("peak=" FLT_FMT(1) " late=%d gap=" FLT_FMT(2)
                     " loops=%d out=" FLT_FMT(3) "/" FLT_FMT(3) " clip=%d"
                     " d=" FLT_FMT(3),
                     FLT_VAR(1, meter_.GetMaxCpuLoad() * 100.f),
                     (int)lateBlocks_, FLT_VAR(2, slots), (int)mainLoops_,
                     FLT_VAR(3, outPeak_), FLT_VAR(3, chunkMin_),
                     (int)clipBlocks_, FLT_VAR(3, maxStep_));
        meter_.Reset();
        lateBlocks_ = 0;
        worstGap_   = 0;
        mainLoops_  = 0;
        outPeak_    = 0.f;
        chunkMin_   = 1.f;
        clipBlocks_ = 0;
        maxStep_    = 0.f;
    }

    float AvgCpu() const { return meter_.GetAvgCpuLoad() * 100.f; }
    float MaxCpu() const { return meter_.GetMaxCpuLoad() * 100.f; }

  private:
    static constexpr uint32_t kChunkBlocks = 128; // ~13 ms at 48k / block 5

    daisy::CpuLoadMeter meter_;

    // Written in the audio IRQ, read in the main loop.
    volatile uint32_t lateBlocks_ = 0;
    volatile uint32_t worstGap_   = 0;
    volatile uint32_t mainLoops_  = 0;
    volatile float    outPeak_    = 0.f;
    volatile float    chunkMin_   = 1.f;
    volatile uint32_t clipBlocks_ = 0;
    volatile float    maxStep_    = 0.f;

    // Audio-IRQ only.
    uint32_t lastTick_    = 0;
    uint32_t lateTicks_   = 0; // 1.5x the nominal block period, in ticks
    float    prevL_       = 0.f;
    float    prevR_       = 0.f;
    float    chunk_       = 0.f;
    uint32_t chunkBlocks_ = 0;
};

} // namespace foxdiag

#else // !FOXTAIL_SERIAL_LOG — every call compiles to nothing.

#include <cstddef>

namespace foxdiag {

class Diag
{
  public:
    void Init(float, int) {}
    void BlockStart() {}
    void BlockEnd(const float*, const float*, size_t) {}
    void MainLoopTick() {}
    template <typename Hw>
    void PrintStatus(Hw&) {}
    float AvgCpu() const { return 0.f; }
    float MaxCpu() const { return 0.f; }
};

} // namespace foxdiag

#endif
