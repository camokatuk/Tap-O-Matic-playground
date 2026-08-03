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

    std::printf("\n%s\n", g_fail ? "FAILED" : "slot table ok");
    return g_fail ? 1 : 0;
}
