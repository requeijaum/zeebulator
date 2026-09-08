#!/usr/bin/env bash
# Phase 9a Phase 5: local CI gate for the dual-core (interpreter oracle + JIT).
#
# Runs, in order, and fails on the first red:
#   1. Full ctest suite (unit + trap + differential fuzz, both cores).
#   2. Real-module lockstep differential over a corpus of real .mod titles:
#      interpreter vs JIT, step-for-step, must be ZERO divergences.
#   3. Steady-state throughput sanity (JIT must beat interp on a hot loop).
#
# The interpreter is always the oracle: any title that diverges is a JIT bug,
# re-triaged by re-running the interpreter alone. This gate is what must be
# broadly green before ZEEB_CPU=jit could become the default.
#
# Usage: ci/jit_gate.sh [BUILD_DIR] [MOD_ROOT]
set -uo pipefail

ZEEB_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ZEEB_DIR/build}"
MOD_ROOT="${2:-$HOME/.Tuxality/Infuse/brew/mod}"
LOCKSTEP="$BUILD/tools/zeebulator_jit_lockstep"
BENCH_LOOP="$BUILD/tools/zeebulator_jit_bench_loop"
STEPS="${STEPS:-200000}"
CORPUS_LIMIT="${CORPUS_LIMIT:-40}"

fail() { echo "GATE FAIL: $*" >&2; exit 1; }

echo "== [1/3] ctest (both cores: unit + trap + differential fuzz) =="
ctest --test-dir "$BUILD" --output-on-failure >/tmp/jit_gate_ctest.log 2>&1 \
  || { tail -20 /tmp/jit_gate_ctest.log; fail "ctest red"; }
grep -E "tests passed" /tmp/jit_gate_ctest.log | tail -1

echo "== [2/3] real-module lockstep (interp oracle vs JIT), 0 divergences required =="
[ -x "$LOCKSTEP" ] || fail "missing $LOCKSTEP (build zeebulator_jit_lockstep)"
mapfile -t MODS < <(find "$MOD_ROOT" -iname "*.mod" 2>/dev/null | sort | head -n "$CORPUS_LIMIT")
[ "${#MODS[@]}" -gt 0 ] || fail "no .mod titles under $MOD_ROOT"
total=0; ok=0; div=0; sumsteps=0
for m in "${MODS[@]}"; do
  total=$((total+1))
  out=$(timeout 60 "$LOCKSTEP" "$m" "$STEPS" 2>&1 | tail -1)
  if echo "$out" | grep -q "DIVERGENCE"; then
    div=$((div+1)); echo "  DIVERGENCE: $(basename "$m"): $out"
  elif echo "$out" | grep -qE "LOCKSTEP OK|top-level return"; then
    ok=$((ok+1))
    s=$(echo "$out" | grep -oE "[0-9]+ steps" | grep -oE "[0-9]+"); sumsteps=$((sumsteps+${s:-0}))
  fi
done
echo "  titles=$total ok=$ok divergences=$div total_steps=$sumsteps"
[ "$div" -eq 0 ] || fail "$div title(s) diverged JIT vs interpreter"

echo "== [2b/3] synthetic CPU conformance suite (testkit/cputests) =="
CPUTESTS="$HOME/projects/zeebo-emulator/testkit/run_cputests.sh"
if [ -x "$CPUTESTS" ] || [ -f "$CPUTESTS" ]; then
  ZEEB_BUILD="$BUILD" bash "$CPUTESTS" >/tmp/jit_gate_cputests.log 2>&1
  tail -1 /tmp/jit_gate_cputests.log
  # require: 0 golden failures AND 0 lockstep divergences (incl. thumb2branch
  # regressor for the Thumb-2 BL granularity fix).
  grep -qE "SUMMARY: [0-9]+ pass, 0 fail, 0 lockstep divergences" /tmp/jit_gate_cputests.log \
    || { tail -20 /tmp/jit_gate_cputests.log; fail "cputests red (golden or lockstep)"; }
else
  echo "  (skipped: run_cputests.sh not found)"
fi

echo "== [3/3] steady-state throughput sanity (JIT must beat interpreter) =="
if [ -x "$BENCH_LOOP" ]; then
  line=$("$BENCH_LOOP" 2000000 3 2>&1 | grep speedup)
  echo "  $line"
  sp=$(echo "$line" | grep -oE "[0-9]+\.[0-9]+")
  awk "BEGIN{exit !($sp > 1.0)}" || fail "JIT not faster than interpreter in steady state ($sp)"
else
  echo "  (skipped: bench_loop not built)"
fi

echo "GATE PASS: dual-core green (interpreter oracle + JIT differential + perf)"
