#!/bin/bash
# The held lean's step and clamp (code/fgame/bg_lean.h, stock AA 1.11's pmove) against the unit test
# code/fgame/tests/test_lean.cpp, the Mac's stand-in for the Vulkan side's ctest (cmake/tests/lean.cmake; cmake/tests is
# gone here, Phase 7a). Built into build.noindex/leantest/ and run, ~1 s.
#   port/leantest.sh
set -euo pipefail
source "$(dirname "$0")/env.sh"

OUT="$OMM_BUILD/leantest"
mkdir -p "$OUT"
clang++ -std=gnu++17 -O2 -g -Wall -Wextra "$OMM_ROOT/code/fgame/tests/test_lean.cpp" -o "$OUT/test_lean"
"$OUT/test_lean"
