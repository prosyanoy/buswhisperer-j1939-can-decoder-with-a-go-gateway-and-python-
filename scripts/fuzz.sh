#!/usr/bin/env bash
# Builds the library + fuzz target with ASan/UBSan, gcov and trace-pc feedback,
# runs the coverage-guided driver, then prints per-file line coverage of the
# library (c/src/*.c). Exit status is non-zero on any crash or sanitizer report.
set -euo pipefail
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
ITERS=${FUZZ_ITERS:-300000}
SEED=${FUZZ_SEED:-1}
OUT=build/fuzz
rm -rf "$OUT"; mkdir -p "$OUT"

SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
LIBFLAGS="-std=c11 -O1 -g $SAN --coverage -fsanitize-coverage=trace-pc -Ic/include"
objs=()
for f in c/src/*.c fuzz/fuzz_target.c; do
  o="$OUT/$(basename "$f" .c).o"
  $CC $LIBFLAGS -c "$f" -o "$o"
  objs+=("$o")
done
# the driver itself is not instrumented (it defines the coverage callback)
$CC -std=c11 -O1 -g $SAN --coverage -Ic/include -c fuzz/standalone_driver.c -o "$OUT/driver.o"
$CC $SAN --coverage "${objs[@]}" "$OUT/driver.o" -o "$OUT/fuzz_driver"

echo "== running $ITERS iterations (seed $SEED) =="
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 \
  "$OUT/fuzz_driver" -n "$ITERS" -s "$SEED" -o "$OUT" | tee "$OUT/result.txt"

echo "== line coverage of c/src (gcov) =="
( cd "$OUT" && gcov -o . id.o signals.o tp.o candump.o 2>/dev/null ) | awk '
  /^File .*c\/src\// { f=$2; gsub(/\x27/,"",f); want=1; next }
  /^File / { want=0; next }
  want && /^Lines executed:/ { split($2,a,"[:%]"); pct=a[2]; of=$4;
      printf "  %-22s %6.2f%% of %d lines\n", f, pct, of; tot+=pct*of/100; n+=of; want=0 }
  END { if (n) printf "  %-22s %6.2f%% of %d lines\n", "TOTAL", 100*tot/n, n }' | tee "$OUT/coverage.txt"
