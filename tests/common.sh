# Shared by the test scripts. Source it from the repository root, with OUT set.
DOTNET=${DOTNET:-/opt/homebrew/bin/dotnet}
VERSION=10.0.14
export DOTNET_CLI_TELEMETRY_OPTOUT=1
unset DOTNET_GCName DOTNET_GCPath DOTNET_GCHeapHardLimit DOTNET_gcServer MANUALGC_LOG
failures=0

# macOS has no timeout(1).
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

# Not the default ports, so that tests do not disturb a viewer you have open.
WEB_PORT=5721
export MANUALGC_PORT=5720

start_viewer() { # extra viewer arguments
    "$DOTNET" viewer/bin/Release/net10.0/viewer.dll --WebPort $WEB_PORT --GcPort $MANUALGC_PORT "$@" > "$OUT/viewer.log" 2>&1 &
    VIEWER=$!
    trap 'kill $VIEWER 2> /dev/null' EXIT
    for _ in $(seq 100); do curl -s -o /dev/null "http://localhost:$WEB_PORT/" && return; sleep 0.1; done
    echo "FAIL  viewer did not start"; cat "$OUT/viewer.log"; exit 1
}

stop_viewer() { kill $VIEWER; wait $VIEWER 2> /dev/null; trap - EXIT; }

build_project() { # path, log name
    "$DOTNET" build -c Release -v q --nologo "$1" > "$OUT/$2.build.log" 2>&1 || { echo "FAIL  build $1"; cat "$OUT/$2.build.log"; exit 1; }
}

player() { limit 100 "$DOTNET" tests/player/bin/Release/net10.0/player.dll $WEB_PORT "$@"; }

# Runs a command with a viewer and a scripted player, and leaves its exit code in rc.
# VIEWER_ARGS are passed to the viewer.
VIEWER_ARGS=
with_player() { # mode, log name, command...
    local mode=$1 log=$2; shift 2
    start_viewer $VIEWER_ARGS
    player $mode > $OUT/$log.player.log 2>&1 & local playing=$!
    await connected $OUT/$log.player.log
    "$@" > $OUT/$log.log 2>&1; rc=$?
    sleep 0.3  # lets the player see the end of the last round
    stop_viewer
    wait $playing 2> /dev/null
}

can_drive_chrome() {
    command -v node > /dev/null && [ -x "${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}" ]
}

await() { # text, file
    for _ in $(seq 200); do grep -qF -- "$1" "$2" 2> /dev/null && return; sleep 0.05; done
}
