#!/usr/bin/env python3
"""Static cost meter for the Fox Tail engine's hot loops.

control-maps.md rule 5 says estimates lie and objdump doesn't. This runs the
objdump: it compiles foxtail_dsp.h alone for the H750 with the firmware's real
flags, finds the innermost loops in Process and UpdateBlock, and counts the
instructions in each. Reports the delta against a checked-in baseline so a
control-map change shows its CPU cost BEFORE anything is flashed.

Never fails the build; the numbers are advisory. --update rewrites the baseline
after a change that is meant to move them.

Not a substitute for the hardware CPU meter (SERIAL_LOG=1) — it counts static
instructions, not cycles, and knows nothing about VSQRT's unpipelined latency
or memory stalls. Its job is relative: "this edit added N instructions per
partial", which the calibration below turns into a CPU estimate.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "loop_cost.json")
# Overridable so a prototype tree can be A/B'd without touching the repo copy.
INCLUDE = [ROOT]

# Must match the Makefile's foxtail arm (OPT + the libDaisy core flags).
CXX = "arm-none-eabi-g++"
FLAGS = [
    "-mcpu=cortex-m7", "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-mthumb",
    "-std=gnu++14", "-O3", "-fno-math-errno", "-fno-trapping-math",
    "-fno-exceptions", "-fno-rtti", "-g",
]

# Hardware anchor, from control-maps.md's measured table: Cluster at 96
# partials is ~80% CPU over ~31% fixed overhead, so ~49 points are per-partial
# work. Divide that by the instructions the two hot loops actually spend per
# partial per block and you get the price of one added instruction.
ANCHOR_PARTIALS = 96
ANCHOR_PER_PARTIAL_PCT = 49.0
BLOCK_FRAMES = 5  # render loop runs this many times per UpdateBlock


def disassemble(defines):
    """Compile the engine standalone and return {func: [(addr, op, args, line)]}."""
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "probe.cpp")
        obj = os.path.join(tmp, "probe.o")
        with open(src, "w") as f:
            f.write('#include "foxtail_dsp.h"\n'
                    'foxtail::FoxTailOsc osc;\n'
                    'void probe(const foxtail::Controls& c, float* a, float* b,'
                    ' size_t n) { osc.Process(c, a, b, n); }\n')
        cmd = [CXX, "-c", src, "-o", obj, "-I" + INCLUDE[0]] + FLAGS
        cmd += ["-D" + d for d in defines]
        subprocess.run(cmd, check=True)
        text = subprocess.run(
            ["arm-none-eabi-objdump", "-d", "-l", "--inlines", obj],
            check=True, capture_output=True, text=True).stdout

    funcs, cur, stack = collections.OrderedDict(), None, []
    for raw in text.split("\n"):
        m = re.match(r"^[0-9a-f]+ <(.+)>:", raw)
        if m:
            cur, stack = m.group(1), []
            funcs[cur] = []
            continue
        line = raw.strip()
        # objdump prints the innermost source line first, then "inlined by"
        # frames outward; the last one is the line inside the function itself.
        m = re.match(r"^(inlined by )?\S*foxtail_dsp\.h:(\d+)", line)
        if m:
            if line.startswith("inlined by"):
                stack.append(int(m.group(2)))
            else:
                stack = [int(m.group(2))]
        m = re.match(r"\s+([0-9a-f]+):\s+[0-9a-f ]+\t(\S+)\s*(.*)", raw)
        if m and cur is not None:
            funcs[cur].append((int(m.group(1), 16), m.group(2), m.group(3).strip(),
                               stack[-1] if stack else 0))
    return funcs


def innermost_loops(insns):
    """Natural loops with no other loop nested inside them.

    Only CONDITIONAL backward branches count. GCC outlines cold tails (the sign
    pick inside SoftClip, for one) past the end of the function and jumps back
    with a plain `b`, which looks exactly like a back-edge and isn't one.
    """
    edges = set()
    for addr, op, args, _ in insns:
        base = op.split(".")[0]
        if not base.startswith("b") or base in ("b", "bl", "blx", "bx", "bic", "bfi", "bfc"):
            continue
        m = re.match(r"^([0-9a-f]+) ", args)
        if m and int(m.group(1), 16) < addr:
            edges.add((int(m.group(1), 16), addr))
    # GCC's unswitched/peeled copies share a header with several back-edges;
    # keep the widest span per header so one loop is reported once.
    widest = {}
    for lo, hi in edges:
        widest[lo] = max(widest.get(lo, hi), hi)
    spans = sorted(widest.items())
    return [(lo, hi) for lo, hi in spans
            if not any(o != (lo, hi) and o[0] >= lo and o[1] <= hi for o in spans)]


def measure(defines):
    """Loops keyed <function>#<rank by size>, so a baseline survives edits that
    move line numbers -- which is every edit this tool exists to measure. The
    source span rides along as a label only."""
    out = {}
    for fn, insns in disassemble(defines).items():
        if not insns:
            continue
        short = "Process" if "Process" in fn or "probe" in fn else \
                "UpdateBlock" if "UpdateBlock" in fn else fn
        found = {}
        for lo, hi in innermost_loops(insns):
            body = [x for x in insns if lo <= x[0] <= hi]
            lines = [x[3] for x in body if x[3]]
            ops = collections.Counter(x[1].split(".")[0] for x in body)
            span = (min(lines), max(lines)) if lines else (0, 0)
            # GCC unswitches the shaper's mode test into twin loop bodies over
            # the same source span; the costlier twin is the one to budget for.
            if span not in found or found[span]["insns"] < len(body):
                found[span] = {"insns": len(body),
                               "vsqrt": ops["vsqrt"] + ops["vdiv"],
                               "lines": list(span)}
        for rank, (span, v) in enumerate(
                sorted(found.items(), key=lambda kv: -kv[1]["insns"]), 1):
            out["%s#%d" % (short, rank)] = v
    return out


def per_partial(loops):
    """Instructions spent per partial per audio block, across both hot loops."""
    def pick(prefix):
        k = prefix + "#1"
        return (loops[k]["insns"], k) if k in loops else (0, None)
    # The costliest loop in each function is the per-partial one: in Process it
    # is the only loop over kNumPartials, in UpdateBlock it outweighs the 8-turn
    # band loop that shares the function.
    render, render_key = pick("Process")
    update, update_key = pick("UpdateBlock")
    return render * BLOCK_FRAMES + update, render, update, render_key, update_key


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true", help="rewrite the baseline")
    ap.add_argument("-D", dest="defines", action="append", default=[],
                    help="extra -D passed to the compiler (repeatable)")
    ap.add_argument("--root", help="directory holding the foxtail_dsp.h to measure")
    args = ap.parse_args()
    if args.root:
        INCLUDE[0] = os.path.abspath(args.root)

    loops = measure(args.defines)
    total, render, update, render_key, update_key = per_partial(loops)
    pct_per_insn = ANCHOR_PER_PARTIAL_PCT / total if total else 0.0

    base = {}
    if os.path.exists(BASELINE):
        with open(BASELINE) as f:
            base = json.load(f).get("loops", {})
    old_total = base.pop("__total__", None)

    print("Fox Tail hot-loop cost  (-O3, cortex-m7"
          + ("".join(" -D" + d for d in args.defines)) + ")\n")
    print("  %-26s %7s %7s %8s  %s"
          % ("loop", "insns", "delta", "sqrt/div", "source lines"))
    hot = {k for k in (render_key, update_key) if k}
    for key in sorted(loops):
        v = loops[key]
        old = base.get(key, {}).get("insns")
        delta = "  --" if old is None else ("%+d" % (v["insns"] - old) if v["insns"] != old else "0")
        print("  %-26s %7d %7s %8d  %d-%d%s"
              % (key, v["insns"], delta, v["vsqrt"], v["lines"][0], v["lines"][1],
                 "   <- per partial" if key in hot else ""))
    for key in sorted(set(base) - set(loops)):
        print("  %-26s %7s %7s   (gone)" % (key, "-", "-"))

    print("\n  per partial per block: %d instructions "
          "(render %d x%d + block %d)" % (total, render, BLOCK_FRAMES, update))
    if total:
        print("  calibration: 1 instruction in the block loop ~ %.2f%% CPU; "
              "in the render loop ~ %.2f%%" % (pct_per_insn, pct_per_insn * BLOCK_FRAMES))
        print("  one partial costs %.2f%%, so ~%.1f block-loop instructions = 1 partial"
              % (ANCHOR_PER_PARTIAL_PCT / ANCHOR_PARTIALS,
                 (ANCHOR_PER_PARTIAL_PCT / ANCHOR_PARTIALS) / pct_per_insn))

    if old_total and old_total != total:
        print("  vs baseline: %+d instructions per partial -> ~%+.1f CPU points"
              % (total - old_total, (total - old_total) * pct_per_insn))

    if args.update:
        loops["__total__"] = total
        with open(BASELINE, "w") as f:
            json.dump({"note": "regenerate with tools/loop_cost.py --update",
                       "loops": loops}, f, indent=2, sort_keys=True)
            f.write("\n")
        print("\n  baseline written to %s" % os.path.relpath(BASELINE, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())