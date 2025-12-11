#!/bin/bash
# Stage 1 "done when": hello and a sample allocating a few hundred MB run to completion on the
# stock runtime with DOTNET_GCName pointing at the library.
set -u
cd "$(dirname "$0")/.."
DOTNET=${DOTNET:-/opt/homebrew/bin/dotnet}
RUNS=${RUNS:-10}
VERSION=10.0.14
export DOTNET_CLI_TELEMETRY_OPTOUT=1
unset DOTNET_GCName DOTNET_GCPath DOTNET_GCHeapHardLimit DOTNET_gcServer
OUT=artifacts/stage1
failures=0

check() { # name, expected exit code, actual exit code, output file, [text that must appear]
    if [ "$2" = "$3" ] && { [ $# -lt 5 ] || grep -qF -- "$5" "$4"; }; then
        echo "PASS  $1"
    else
        echo "FAIL  $1 (exit $3, wanted $2${5:+, looking for \"$5\"})"; sed 's/^/        /' "$4"; failures=$((failures + 1))
    fi
}

rm -rf $OUT && mkdir -p $OUT
"$DOTNET" --list-runtimes | grep -q "^Microsoft.NETCore.App $VERSION " || { echo "FAIL  .NET $VERSION not installed for $DOTNET"; exit 1; }
make > $OUT/make.log 2>&1 || { echo "FAIL  make"; cat $OUT/make.log; exit 1; }
for sample in hello churn; do
    "$DOTNET" build -c Release -v q --nologo samples/$sample/$sample.csproj > $OUT/$sample.build.log 2>&1 \
        || { echo "FAIL  build $sample"; cat $OUT/$sample.build.log; exit 1; }
    cp artifacts/libmanualgc.dylib samples/$sample/bin/Release/net10.0/
done
HELLO=samples/hello/bin/Release/net10.0/hello.dll
CHURN=samples/churn/bin/Release/net10.0/churn.dll

# Without this the other checks could pass on the stock GC.
export MANUALGC_LOG=1 DOTNET_GCName=libmanualgc.dylib
"$DOTNET" $HELLO > $OUT/hello.log 2>&1; rc=$?
check "hello loads our GC" 0 $rc $OUT/hello.log "[manualgc"
check "hello prints" 0 $rc $OUT/hello.log "Hello from .NET $VERSION"

for i in $(seq "$RUNS"); do
    "$DOTNET" $CHURN > $OUT/churn.$i.log 2>&1; rc=$?
    check "churn run $i" 0 $rc $OUT/churn.$i.log "[manualgc"
done
grep -E "heap grew|EE suspensions [0-9]" $OUT/churn.1.log | sed 's/^/        /'

DOTNET_GCHeapHardLimit=0x8000000 "$DOTNET" $CHURN oom > $OUT/oom.log 2>&1; rc=$?
check "out of memory is catchable (128 MB limit)" 0 $rc $OUT/oom.log "caught OutOfMemoryException"
DOTNET_gcServer=1 "$DOTNET" $HELLO > $OUT/server.log 2>&1; rc=$?
check "server GC is refused" 137 $rc $OUT/server.log "server GC is not supported"
DOTNET_GCName=missing.dylib "$DOTNET" $HELLO > $OUT/missing.log 2>&1; rc=$?
check "a missing library fails cleanly" 137 $rc $OUT/missing.log "GC initialization failed"

[ $failures = 0 ] && echo "stage 1: all checks passed" || echo "stage 1: $failures check(s) failed"
exit $((failures > 0))
