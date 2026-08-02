#!/bin/sh
# Build and run the engine tests. -std=c++14 on purpose: it doubles as the
# check that foxtail_dsp.h stays buildable by the firmware toolchain.
set -e
cd "$(dirname "$0")"
mkdir -p ../build/tests
c++ -std=c++14 -O2 -Wall -Wextra -o ../build/tests/clip_sweep clip_sweep.cpp
../build/tests/clip_sweep "$@"