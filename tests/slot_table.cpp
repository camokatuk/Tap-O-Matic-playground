// Invariants of the pan-slot table in foxtail_dsp.h.
//
// kSlotPat is searched, not derived, so nothing in the source explains why any
// particular row is correct. These are the three properties the search
// optimised for; a hand-edit that breaks one is silent otherwise — the module
// still makes sound, it just loses the stereo image in some switch position.

#include "../foxtail_dsp.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

namespace {

int g_fail = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok) ++g_fail;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

// Pan position of level j when n levels are in use, matching UpdateBlock.
float Pos(int j, int n) { return n > 1 ? 2.f * (float)j / (float)(n - 1) - 1.f : 0.f; }

} // namespace

int main()
{
    const int kN = foxtail::kNumBands; // rows == kSlots == 8
    char      buf[160];

    for (int n = 1; n <= kN; ++n)
    {
        const unsigned char* row = foxtail::kSlotPat[n - 1];

        // 1. Every position is reachable. A row that skips one wastes the width
        //    the pot is asking for; the degenerate case (all zeros) is mono.
        std::set<int> used(row, row + 8);
        for (int j = 0; j < 8; ++j)
            if (row[j] >= n) used.insert(99); // out of range for this n
        std::snprintf(buf, sizeof buf, "n=%d uses all %d positions, none out of range",
                      n, n);
        Check((int)used.size() == n && used.count(99) == 0, buf);

        // 2. Even-k and odd-k partials each stay balanced around centre. Switch 2
        //    in ODD ONLY silences one parity, so an unbalanced subset means the
        //    image jumps sideways when the switch flips.
        //    Exact cancellation is reachable for every n, so this is == 0, not a
        //    tolerance. It is what forces the row's multiset to be symmetric:
        //    the naive j % 8 fill leans left for any n that does not divide 8.
        float even = 0.f, odd = 0.f;
        for (int k = 0; k < 8; k += 2) even += Pos(row[k], n);
        for (int k = 1; k < 8; k += 2) odd += Pos(row[k], n);
        std::snprintf(buf, sizeof buf, "n=%d parity balance even=%+.2f odd=%+.2f",
                      n, even, odd);
        Check(std::fabs(even) < 1e-4f && std::fabs(odd) < 1e-4f, buf);

        // 3. Neighbouring partials do not share a position. Adjacent harmonics
        //    panned together correlate and the fan collapses into blobs.
        //    n=1 is mono by definition. n=2 is the one real exception: a
        //    2-colouring that alternates on consecutive k IS parity, so it would
        //    put every ODD ONLY survivor on one side. Balance wins; two repeats
        //    out of eight is the best a balanced row can do.
        int adjacent = 0;
        for (int k = 0; k < 8; ++k)
            if (row[k] == row[(k + 1) % 8]) ++adjacent;
        const int allowed = n == 1 ? 8 : (n == 2 ? 2 : 0);
        std::snprintf(buf, sizeof buf, "n=%d neighbours sharing a slot: %d (allowed %d)",
                      n, adjacent, allowed);
        Check(adjacent <= allowed, buf);
    }

    // The pan anchors reuse the n=8 row, so each one has to hold invariant 2 in
    // its own positions.
    const unsigned char* row = foxtail::kSlotPat[kN - 1];
    for (int a = 0; a < foxtail::kPanAnchors; ++a)
    {
        float even = 0.f, odd = 0.f;
        for (int k = 0; k < 8; k += 2) even += foxtail::kPanAnchor[a][row[k]];
        for (int k = 1; k < 8; k += 2) odd += foxtail::kPanAnchor[a][row[k]];
        std::snprintf(buf, sizeof buf, "anchor %d parity balance even=%+.2f odd=%+.2f",
                      a, even, odd);
        Check(std::fabs(even) < 1e-4f && std::fabs(odd) < 1e-4f, buf);
    }

    // Nothing may reach a hard pan: that is what keeps the peak profile flat
    // across the knob, and it is a property of the anchors, not of the morph.
    {
        float widest = foxtail::kPanRotAmp;
        for (int a = 0; a < foxtail::kPanAnchors; ++a)
            for (int i = 0; i < 8; ++i)
                if (std::fabs(foxtail::kPanAnchor[a][i]) > widest)
                    widest = std::fabs(foxtail::kPanAnchor[a][i]);
        std::snprintf(buf, sizeof buf, "widest pan position %.3f (SCATTER may touch 1.0)",
                      widest);
        Check(widest <= 1.f + 1e-6f, buf);
    }

    // ORBIT moves, and its two parity subsets run at DIFFERENT rates, so their
    // phases are independent. Balance therefore has to hold for every
    // combination of the two, not just where they happen to line up -- checked
    // on a grid rather than argued from the table.
    {
        float worstEven = 0.f, worstOdd = 0.f;
        for (int sa = 0; sa < 64; ++sa)
            for (int sb = 0; sb < 64; ++sb)
            {
                const float pa = (float)sa / 64.f, pb = (float)sb / 64.f;
                float       even = 0.f, odd = 0.f;
                for (int k = 0; k < 8; ++k)
                {
                    const int   sl = row[k];
                    const float ph = foxtail::kPanRotSub[sl] ? pb : pa;
                    const float v  = foxtail::kPanRotAmp
                                    * std::sin(6.283185307f
                                               * (ph + foxtail::kPanRotOfs[sl]));
                    if (k % 2 == 0) even += v;
                    else odd += v;
                }
                if (std::fabs(even) > worstEven) worstEven = std::fabs(even);
                if (std::fabs(odd) > worstOdd) worstOdd = std::fabs(odd);
            }
        std::snprintf(buf, sizeof buf,
                      "ORBIT parity balance over both phases: worst even=%.4f odd=%.4f",
                      worstEven, worstOdd);
        Check(worstEven < 1e-4f && worstOdd < 1e-4f, buf);
    }

    // Each subset must own exactly the slots of one k-parity, or the two rates
    // would split a subset and the balance above would not survive detuning.
    {
        bool ok = true;
        for (int k = 0; k < 8; ++k)
            if (foxtail::kPanRotSub[row[k]] != (unsigned char)(k & 1)) ok = false;
        std::snprintf(buf, sizeof buf, "ORBIT subsets follow k parity exactly");
        Check(ok, buf);
    }

    // Offsets must stay evenly spread, or the slots bunch and the orbit stops
    // covering the field.
    {
        std::set<int> eighths;
        for (int i = 0; i < 8; ++i)
            eighths.insert((int)(foxtail::kPanRotOfs[i] * 8.f + 0.5f) & 7);
        std::snprintf(buf, sizeof buf, "ORBIT offsets use all 8 eighths (%d distinct)",
                      (int)eighths.size());
        Check(eighths.size() == 8, buf);
    }

    // Knots must ascend, or the segment search picks a zero-width span and
    // divides by zero.
    for (int a = 0; a + 1 < foxtail::kPanAnchors; ++a)
    {
        std::snprintf(buf, sizeof buf, "knot %d (%.2f) < knot %d (%.2f)",
                      a, foxtail::kPanKnot[a], a + 1, foxtail::kPanKnot[a + 1]);
        Check(foxtail::kPanKnot[a] < foxtail::kPanKnot[a + 1], buf);
    }

    std::printf("\n%s\n", g_fail ? "FAILED" : "slot table ok");
    return g_fail ? 1 : 0;
}
