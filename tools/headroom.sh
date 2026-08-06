#!/bin/sh
# Build and run the headroom report. Deliberately not part of tests/run.sh:
# that is the regression suite, this is a tuning tool.
set -e
cd "$(dirname "$0")"
mkdir -p ../build/tools
c++ -std=c++14 -O2 -Wall -Wextra -pthread -o ../build/tools/headroom headroom.cpp
../build/tools/headroom "$@"
