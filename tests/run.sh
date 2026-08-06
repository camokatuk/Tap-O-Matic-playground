#!/bin/sh
# Build and run the engine tests. -std=c++14 on purpose: it doubles as the
# check that foxtail_dsp.h stays buildable by the firmware toolchain.
#
# clip_guard is the pass/fail gate. It builds twice: once as shipped, once
# with FOXTAIL_CLUSTER_NORM=0 (compensation defeated) to prove its witness
# patches genuinely clip without the compensation — a guard against the
# NORM=1 assertions passing vacuously. It shards its sweeps across cores.
#
# clip_sweep asserts nothing — it is the detailed report, and everything it
# covers clip_guard covers on a finer grid with a verdict. Run it on demand:
#   ./run.sh --sweep
set -e
cd "$(dirname "$0")"
mkdir -p ../build/tests

CXX_FLAGS="-std=c++14 -O2 -Wall -Wextra -pthread"

sweep=0
if [ "$1" = "--sweep" ]; then
    sweep=1
    shift
fi

pids=""
c++ $CXX_FLAGS -DFOXTAIL_CLUSTER_NORM=0 -o ../build/tests/clip_guard_raw clip_guard.cpp &
pids="$pids $!"
c++ $CXX_FLAGS -o ../build/tests/clip_guard clip_guard.cpp &
pids="$pids $!"
c++ $CXX_FLAGS -o ../build/tests/slot_table slot_table.cpp &
pids="$pids $!"
if [ $sweep = 1 ]; then
    c++ $CXX_FLAGS -o ../build/tests/clip_sweep clip_sweep.cpp &
    pids="$pids $!"
fi
for p in $pids; do wait "$p"; done

../build/tests/slot_table

../build/tests/clip_guard_raw
echo
../build/tests/clip_guard

if [ $sweep = 1 ]; then
    echo
    ../build/tests/clip_sweep "$@"
fi
