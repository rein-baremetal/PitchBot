#!/bin/sh
# Builds and runs every engine check on a desktop (g++ 9+ or clang).  Usage: sh run_tests.sh
set -e
cd "$(dirname "$0")"
HERE=$(pwd)
SRC=../app/src/main/cpp
OUT=${TMPDIR:-/tmp}/pitchbot_tests
mkdir -p "$OUT"
for t in test_engine scoring_checks rescue_checks rules_checks edge_checks fix_checks meta_checks upgrade_checks tactics_checks stun_checks track_checks; do
  g++ -std=c++17 -O1 -I$SRC $t.cpp $SRC/pitch_engine.cpp $SRC/engine.cpp -o "$OUT/$t"
  printf '%-16s ' "$t"
  (cd "$OUT" && ./$t "$HERE/real_frames/track_excerpt.csv" 2>&1 | grep -c '^FAIL' | sed 's/^/failures: /') || true
done
echo "replay: g++ -std=c++17 -O2 -I$SRC replay_tracks.cpp $SRC/pitch_engine.cpp $SRC/engine.cpp -o replay && ./replay <track.csv>"
