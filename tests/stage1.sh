#!/bin/bash
# Stage 1 "done when": hello and a sample allocating a few hundred MB run to completion on the
# stock runtime with DOTNET_GCName pointing at the library.
set -u
cd "$(dirname "$0")/.."
OUT=artifacts/stage1
RUNS=${RUNS:-10}
. tests/common.sh
build hello churn
HELLO=$(sample hello)
CHURN=$(sample churn)

# Without the log line the other checks could pass on the stock GC.
export MANUALGC_LOG=1 DOTNET_GCName=libmanualgc.dylib
limit 120 "$DOTNET" $HELLO > $OUT/hello.log 2>&1; rc=$?
check "hello loads our GC" 0 $rc $OUT/hello.log "[manualgc"
check "hello prints" 0 $rc $OUT/hello.log "Hello from .NET $VERSION"

for i in $(seq "$RUNS"); do
    limit 120 "$DOTNET" $CHURN > $OUT/churn.$i.log 2>&1; rc=$?
    check "churn run $i" 0 $rc $OUT/churn.$i.log "[manualgc"
done
grep -E "heap grew|EE suspensions [0-9]" $OUT/churn.1.log | sed 's/^/        /'

DOTNET_GCHeapHardLimit=0x8000000 limit 120 "$DOTNET" $CHURN oom > $OUT/oom.log 2>&1; rc=$?
check "out of memory is catchable (128 MB limit)" 0 $rc $OUT/oom.log "caught OutOfMemoryException"
DOTNET_gcServer=1 limit 120 "$DOTNET" $HELLO > $OUT/server.log 2>&1; rc=$?
check "server GC is refused" 137 $rc $OUT/server.log "server GC is not supported"
DOTNET_GCName=missing.dylib limit 120 "$DOTNET" $HELLO > $OUT/missing.log 2>&1; rc=$?
check "a missing library fails cleanly" 137 $rc $OUT/missing.log "GC initialization failed"
finish "stage 1"
