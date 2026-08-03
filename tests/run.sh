#!/bin/sh
# Build and run the engine tests. -std=c++14 on purpose: it doubles as the
# check that foxtail_dsp.h stays buildable by the firmware toolchain.
#
# clip_guard is the pass/fail gate. It builds twice: once as shipped, once
# with FOXTAIL_CLUSTER_NORM=0 (compensation defeated) to prove its witness
# patches genuinely clip without the compensation — a guard against the
# NORM=1 assertions passing vacuously. clip_sweep is the detailed report.
set -e
cd "$(dirname "$0")"
mkdir -p ../build/tests

CXX_FLAGS="-std=c++14 -O2 -Wall -Wextra"

c++ $CXX_FLAGS -DFOXTAIL_CLUSTER_NORM=0 -o ../build/tests/clip_guard_raw clip_guard.cpp
c++ $CXX_FLAGS -o ../build/tests/clip_guard clip_guard.cpp
c++ $CXX_FLAGS -o ../build/tests/clip_sweep clip_sweep.cpp
c++ $CXX_FLAGS -o ../build/tests/slot_table slot_table.cpp

../build/tests/slot_table

../build/tests/clip_guard_raw
echo
../build/tests/clip_guard
echo
../build/tests/clip_sweep "$@"
