#!/bin/bash
# Stage 4 "done when": memory is reused after erasing, verified by a sample that would otherwise
# run out of a small fixed heap limit.
set -u
cd "$(dirname "$0")/.."
OUT=artifacts/stage4
SECONDS_OF_STRESS=${SECONDS_OF_STRESS:-15}
. tests/common.sh
build ration stress orchard garbage
build_project viewer/viewer.csproj viewer
build_project tests/player/player.csproj player

with_player() { # mode, log name, command...
    local mode=$1 log=$2; shift 2
    start_viewer
    player $mode > $OUT/$log.player.log 2>&1 & local playing=$!
    await connected $OUT/$log.player.log
    "$@" > $OUT/$log.log 2>&1; rc=$?
    stop_viewer
    wait $playing 2> /dev/null
}
gc() { env DOTNET_GCName=libmanualgc.dylib MANUALGC_LOG=1 "$@"; }
app() { gc perl -e 'alarm 100; exec @ARGV' "$DOTNET" "$@"; }
limited() { gc perl -e 'alarm 100; exec @ARGV' env DOTNET_GCHeapHardLimit=0x2000000 "$DOTNET" "$@"; }

limited $(sample ration) > $OUT/noviewer.log 2>&1; rc=$?
# With no room left the sample may not even be able to report it, so it can also die here.
[ $rc != 0 ] && rc=failed
check "32 MB limit, no viewer: runs out of memory" failed $rc $OUT/noviewer.log "ut of memory"
with_player idle idle limited $(sample ration)
[ $rc != 0 ] && rc=failed
check "32 MB limit, nothing erased: runs out of memory" failed $rc $OUT/idle.log "ut of memory"
with_player clean clean limited $(sample ration)
check "32 MB limit, garbage erased: finishes" 0 $rc $OUT/clean.log "finished 20 rounds, 80 MB of garbage"
check "    live data intact" 0 $rc $OUT/clean.log "2000 live objects intact"
check "    weak references to garbage cleared" 0 $rc $OUT/clean.log "0 of 20 weak references alive"
check "    nothing refused" 0 $rc $OUT/clean.player.log "refused 0"
grep -E "^finished|exit," $OUT/clean.log | sed 's/^/        /'
with_player clean long limited $(sample ration) 200 8
check "32 MB limit, 1.6 GB of garbage: finishes" 0 $rc $OUT/long.log "finished 200 rounds, 1600 MB of garbage"
grep -E "^finished|exit," $OUT/long.log | sed 's/^/        /'
with_player random random limited $(sample ration)
check "32 MB limit, half the garbage erased each time: finishes" 0 $rc $OUT/random.log "finished 20 rounds"

with_player greedy greedy app $(sample ration) 1
check "erasing live objects is refused and the app survives" 0 $rc $OUT/greedy.log "100 live objects intact"
check "    three lives, and none of the rest of the batch" 0 $rc $OUT/greedy.player.log "refused 3, freed 0, lives 0"
check "    nothing can be erased without lives" 0 $rc $OUT/greedy.log "erased 0 objects 0 bytes, refused 3"

for mode in clean random; do
    MANUALGC_POISON=1 with_player $mode stress-$mode app $(sample stress) $SECONDS_OF_STRESS
    check "threads mutating while garbage is erased and poisoned ($mode)" 0 $rc $OUT/stress-$mode.log " 0 failures"
    grep operations $OUT/stress-$mode.log | sed 's/^/        /'
done

start_viewer
app $(sample orchard) > $OUT/killed.log 2>&1 & APP=$!
await "Collection 1" $OUT/viewer.log
sleep 0.5
grep -q resumed $OUT/killed.log && echo "the app did not wait" >> $OUT/killed.log
stop_viewer
wait $APP; rc=$?
check "killing the viewer mid-collection resumes the app" 0 $rc $OUT/killed.log "resuming the app without a Done"
! grep -q "did not wait" $OUT/killed.log; check "    and it was waiting until then" 0 $? $OUT/killed.log

if command -v node > /dev/null && [ -x "${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}" ]; then
    start_viewer
    app $(sample orchard) > $OUT/page.app.log 2>&1 & APP=$!
    await "Collection 1" $OUT/viewer.log
    limit 60 node tests/page4.mjs http://localhost:$WEB_PORT/ $OUT/page.png > $OUT/page.log 2>&1; rc=$?
    check "mouse drag frees garbage, live click costs a life, Done resumes ($OUT/page.png)" 0 $rc $OUT/page.log "ok"
    stop_viewer
    wait $APP; rc=$?
    check "    app finishes" 0 $rc $OUT/page.app.log "resumed"
else
    echo "SKIP  page check (needs node and Chrome)"
fi
finish "stage 4"
