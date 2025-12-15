#!/bin/bash
# Stage 2 "done when": the log of live and dead counts and bytes matches expectations on a
# sample with known garbage.
set -u
cd "$(dirname "$0")/.."
OUT=artifacts/stage2
RUNS=${RUNS:-5}
. tests/common.sh
build garbage churn

export DOTNET_GCName=libmanualgc.dylib
for i in $(seq "$RUNS"); do
    MANUALGC_LOG=2 limit 120 "$DOTNET" $(sample garbage) > $OUT/garbage.$i.out 2> $OUT/garbage.$i.err; rc=$?
    check "garbage run $i" 0 $rc $OUT/garbage.$i.out "interior targets usable: True"
    python3 tests/compare.py $OUT/garbage.$i.out $OUT/garbage.$i.err > $OUT/compare.$i.log; rc=$?
    check "garbage run $i: $(tail -1 $OUT/compare.$i.log)" 0 $rc $OUT/compare.$i.log
done
grep " live .* walked " $OUT/garbage.1.err | sed 's/^/        /'

# Marks under load: any reference to something that is not an object ends the process.
for i in $(seq "$RUNS"); do
    MANUALGC_LOG=1 limit 120 "$DOTNET" $(sample churn) collect > $OUT/churn.$i.log 2>&1; rc=$?
    check "churn run $i with two threads collecting: $(grep -c ' walked ' $OUT/churn.$i.log) collections" 0 $rc $OUT/churn.$i.log "collections requested: $(grep -c ' walked ' $OUT/churn.$i.log)"
done
finish "stage 2"
