# Shared by the stage scripts. Source it from the repository root.
DOTNET=${DOTNET:-/opt/homebrew/bin/dotnet}
VERSION=10.0.14
export DOTNET_CLI_TELEMETRY_OPTOUT=1
unset DOTNET_GCName DOTNET_GCPath DOTNET_GCHeapHardLimit DOTNET_gcServer MANUALGC_LOG
failures=0

limit() { perl -e 'alarm shift; exec @ARGV or exit 127' "$@"; } # seconds, command

check() { # name, expected exit code, actual exit code, output file, [text that must appear]
    if [ "$2" = "$3" ] && { [ $# -lt 5 ] || grep -qF -- "$5" "$4"; }; then
        echo "PASS  $1"
    else
        echo "FAIL  $1 (exit $3, wanted $2${5:+, looking for \"$5\"})"; tail -40 "$4" | sed 's/^/        /'
        failures=$((failures + 1))
    fi
}

build() { # samples to build next to the library
    rm -rf "$OUT" && mkdir -p "$OUT"
    "$DOTNET" --list-runtimes | grep -q "^Microsoft.NETCore.App $VERSION " || { echo "FAIL  .NET $VERSION not installed for $DOTNET"; exit 1; }
    make > "$OUT/make.log" 2>&1 || { echo "FAIL  make"; cat "$OUT/make.log"; exit 1; }
    for sample in "$@"; do
        "$DOTNET" build -c Release -v q --nologo "samples/$sample/$sample.csproj" > "$OUT/$sample.build.log" 2>&1 \
            || { echo "FAIL  build $sample"; cat "$OUT/$sample.build.log"; exit 1; }
        cp artifacts/libmanualgc.dylib "samples/$sample/bin/Release/net10.0/"
    done
}

sample() { echo "samples/$1/bin/Release/net10.0/$1.dll"; }

finish() {
    [ $failures = 0 ] && echo "$1: all checks passed" || echo "$1: $failures check(s) failed"
    exit $((failures > 0))
}
