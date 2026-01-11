#!/bin/bash
# The game: scoring, lives, the timer, and hardcore mode, which must be off by default, need the app's
# permission and say that it crashes the app. Also what a hostile viewer can do (nothing).
set -u
cd "$(dirname "$0")/.."
OUT=artifacts/stage5
. tests/common.sh
unset MANUALGC_HARDCORE
build ration orchard
build_project viewer/viewer.csproj viewer
build_project tests/player/player.csproj player

app() { env DOTNET_GCName=libmanualgc.dylib MANUALGC_LOG=1 perl -e 'alarm 60; exec @ARGV' "$DOTNET" "$@"; }

# 200 arrays of 500 to 699 bytes: each takes 24 + length rounded up to 8.
want=$(python3 -c 'print(sum((24 + n + 7) // 8 * 8 for n in range(500, 700)))')
with_player bytes score app $(sample orchard) 2
check "app finishes" 0 $rc $OUT/score.log "resumed"
check "round 1 scores the bytes freed ($want)" 0 $rc $OUT/score.player.log "\"score\":$want,\"totalScore\":$want,"
check "round 2 adds to the total" 0 $rc $OUT/score.player.log "\"totalScore\":$((want * 2)),"
check "no lives lost" 0 $rc $OUT/score.player.log '"lives":3'
with_player clean peek app $(sample orchard) 1
check "peeking at what is alive scores nothing" 0 $rc $OUT/peek.player.log '"peeked":true,"hardcore":false,"score":0'
check "    and all garbage was found" 0 $rc $OUT/peek.player.log 'resumed: done, missed 0,'

VIEWER_ARGS="--Seconds 2"
start=$(date +%s)
with_player wait timer app $(sample orchard) 1
elapsed=$(($(date +%s) - start))
check "time runs out and the app resumes" 0 $rc $OUT/timer.player.log "resumed: time"
[ $elapsed -ge 2 ] && [ $elapsed -le 8 ]; check "    after about 2 seconds (whole run took $elapsed)" 0 $? $OUT/timer.log
VIEWER_ARGS=

with_player hardcore refused app $(sample ration) 1
check "hardcore is refused unless the app allows it" 0 $rc $OUT/refused.player.log "destroyed 0, refused 3"
check "    and live data is intact" 0 $rc $OUT/refused.log "100 live objects intact"
export MANUALGC_HARDCORE=1
with_player bytes allowed app $(sample orchard) 1
check "allowed but not switched on: nothing is destroyed" 0 $rc $OUT/allowed.player.log '"destroyed":0,"peeked":false,"hardcore":false'
with_player hardcore hardcore app $(sample ration) 5
[ $rc != 0 ] && rc=broken
check "allowed and switched on: 100 live objects freed and the app breaks" broken $rc $OUT/hardcore.player.log "destroyed 100, refused 0"
grep -E "destroyed|damaged|Fatal|Unhandled|error" $OUT/hardcore.log | head -3 | sed 's/^/        /'
unset MANUALGC_HARDCORE

for attack in force bogus garbled huge silent; do
    limit 60 python3 tests/rogue.py $MANUALGC_PORT $attack > $OUT/rogue.$attack.log 2>&1 & ROGUE=$!
    await listening $OUT/rogue.$attack.log
    app $(sample ration) 3 2 > $OUT/rogue.$attack.app.log 2>&1; rc=$?
    check "hostile viewer ($attack): app unharmed" 0 $rc $OUT/rogue.$attack.app.log "300 live objects intact"
    wait $ROGUE; check "    GC refused" 0 $? $OUT/rogue.$attack.log ok
done

if can_drive_chrome; then
    for mode in safe hardcore; do
        [ $mode = hardcore ] && export MANUALGC_HARDCORE=1
        start_viewer
        app $(sample orchard) > $OUT/page.$mode.app.log 2>&1 & APP=$!
        await "Collection 1" $OUT/viewer.log
        limit 60 node tests/page5.mjs http://localhost:$WEB_PORT/ $OUT/page.$mode.png $mode > $OUT/page.$mode.log 2>&1; rc=$?
        check "page in $mode mode: $(head -1 $OUT/page.$mode.log)" 0 $rc $OUT/page.$mode.log "ok"
        stop_viewer
        wait $APP; check "    app finishes" 0 $? $OUT/page.$mode.app.log "resumed"
    done
else
    echo "SKIP  page checks (need node and Chrome)"
fi
finish "stage 5"
