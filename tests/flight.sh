#!/bin/bash
# The flight game. A player who follows the ropes correctly must find all of the garbage and hit nothing
# alive, and the page itself must be playable.
set -u
cd "$(dirname "$0")/.."
OUT=artifacts/flight
. tests/common.sh
build airfield orchard ration stress
build_project viewer/viewer.csproj viewer
build_project tests/player/player.csproj player

app() { env DOTNET_GCName=libmanualgc.dylib MANUALGC_LOG=1 perl -e 'alarm 120; exec @ARGV' "$DOTNET" "$@"; }
limited() { env DOTNET_GCHeapHardLimit=0x2000000 DOTNET_GCName=libmanualgc.dylib perl -e 'alarm 100; exec @ARGV' "$DOTNET" "$@"; }
graphs() { # log: every graph agreed with the GC, every round found all garbage, nothing was refused
    local log=$OUT/$1.player.log
    local all=$(grep -c '^graph ' $log) agreed=$(grep -c ' 0 disagreements' $log) clean=$(grep -c '^resumed: done, missed 0,' $log)
    echo "$all graphs, $agreed agree with the GC, $clean rounds left no garbage" >> $log
    [ $all -gt 0 ] && [ $all = $agreed ] && [ $all = $clean ] && grep -q ' refused 0$' $log
    check "$1: $all graphs, $agreed agree with the GC, $clean rounds left no garbage" 0 $? $log
}

with_player flight airfield app $(sample airfield) 5
check "airfield finishes" 0 $rc $OUT/airfield.log "everything kept is intact"
! grep -q DAMAGED $OUT/airfield.log; check "    undamaged every round" 0 $? $OUT/airfield.log
graphs airfield
with_player flight orchard app $(sample orchard) 5
check "orchard finishes" 0 $rc $OUT/orchard.log "resumed"
graphs orchard
with_player flight ration limited $(sample ration)
check "ration finishes inside 32 MB" 0 $rc $OUT/ration.log "2000 live objects intact"
graphs ration
MANUALGC_POISON=1 with_player flight stress app $(sample stress) 10 100
check "threads mutating while the graph is played" 0 $rc $OUT/stress.log " 0 failures"
graphs stress

with_player strafe strafe app $(sample airfield) 1
check "a live balloon of many objects costs one life" 0 $rc $OUT/strafe.player.log '"live":true,"count":0,"bytes":0}] {"lives":2'
check "a wreck costs one life" 0 $rc $OUT/strafe.player.log 'after crash: {"lives":1'

if can_drive_chrome; then
    start_viewer
    app $(sample airfield) 1 > $OUT/page.app.log 2>&1 & APP=$!
    await "Collection 1" $OUT/viewer.log
    limit 100 node tests/page6.mjs http://localhost:$WEB_PORT/flight.html $OUT/ > $OUT/page.log 2>&1; rc=$?
    check "the page plays a round with real keys" 0 $rc $OUT/page.log "ok"
    sed 's/^/        /' $OUT/page.log
    stop_viewer
    wait $APP; check "    app finishes undamaged" 0 $? $OUT/page.app.log "everything kept is intact"
else
    echo "SKIP  page check (needs node and Chrome)"
fi
finish "flight"
