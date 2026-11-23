#!/bin/bash
# The stock 1.11 rules' unit tests (openmohaa-server's ctest, the Vulkan side's cmake/tests: lean, pm_flags, spawn_pick,
# farplane, bullet_aa, bounce_aa, masks_aa, crate, ladder; cmake/tests is gone here, Phase 7a). Each pins a pure header
# (code/fgame/bg_lean.h, g_spawnpick.h, g_bulletaa.h, g_bounceaa.h, g_crate.h, g_ladder.h, code/server/sv_farplane.h, the
# masks of code/fgame/bg_public.h, code/qcommon/bg_compat.cpp's pm_flags) to stock. Built into build.noindex/stocktest/
# and run, ~5 s.
#   port/stocktest.sh
set -euo pipefail
source "$(dirname "$0")/env.sh"

OUT="$OMM_BUILD/stocktest"
mkdir -p "$OUT"
C="$OMM_ROOT/code"
CXX=(clang++ -std=gnu++17 -O2 -g -w -I"$C" -I"$C/qcommon")

for t in fgame/tests/test_lean fgame/tests/test_spawn_pick fgame/tests/test_bullet_aa fgame/tests/test_bounce_aa \
	fgame/tests/test_crate fgame/tests/test_ladder fgame/tests/test_masks_aa server/tests/test_farplane; do
	"${CXX[@]}" "$C/$t.cpp" -o "$OUT/$(basename "$t")"
	"$OUT/$(basename "$t")"
done

clang -std=gnu17 -O2 -g -w -I"$C" -I"$C/qcommon" -c "$C/qcommon/q_shared.c" -o "$OUT/q_shared.o"
clang -std=gnu17 -O2 -g -w -I"$C" -I"$C/qcommon" -c "$C/qcommon/common_light.c" -o "$OUT/common_light.o"
"${CXX[@]}" "$C/qcommon/tests/test_pm_flags.cpp" "$OUT/q_shared.o" "$OUT/common_light.o" -o "$OUT/test_pm_flags"
"$OUT/test_pm_flags"
