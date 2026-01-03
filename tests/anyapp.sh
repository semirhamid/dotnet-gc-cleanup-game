#!/bin/bash
# Any .NET web app: untouched template apps, apps built for older .NET, and one with a hard memory
# limit, all started with ./play, frozen from the viewer, and played while serving requests.
set -u
cd "$(dirname "$0")/.."
OUT=artifacts/anyapp
. tests/common.sh
export WEB_PORT DOTNET MANUALGC_LOG=1
APP_PORT=5741
URL=http://localhost:$APP_PORT
rm -rf $OUT && mkdir -p $OUT
build_project tests/player/player.csproj player

start_app() { # log, play arguments...
    local log=$OUT/$1.log; shift
    ./play "$@" --urls $URL > $log 2>&1 & PLAY=$!
    for _ in $(seq 300); do curl -s -o /dev/null $URL/ && return; kill -0 $PLAY 2> /dev/null || return; sleep 0.2; done
}
stop_app() {
    kill -TERM $(pgrep -P $PLAY -f -- "--urls $URL") 2> /dev/null
    wait $PLAY 2> /dev/null; rc=$?
    for _ in $(seq 100); do curl -s -o /dev/null "http://localhost:$WEB_PORT/" || break; sleep 0.1; done
    kill $PLAYER 2> /dev/null; wait $PLAYER 2> /dev/null
}
start_player() { # log, milliseconds between collections
    player flight $2 > $OUT/$1.player.log 2>&1 & PLAYER=$!
    await connected $OUT/$1.player.log
    await '"pid"' $OUT/$1.player.log
}
load() { # requests, path with {} for the request number; prints "count status"
    seq $1 | xargs -P 8 -I{} curl -s --max-time 60 -o /dev/null -w '%{http_code}\n' "$URL$2" | sort | uniq -c | awk '{print $1, $2}' | paste -sd' ' -
}
served() { # name, log, wanted, got
    echo "$4" > $OUT/$2.served.log
    [ "$3" = "$4" ]; check "$1: answered $4" 0 $? $OUT/$2.served.log
}
played() { # log: rounds were played, agreed with the GC, refused nothing, freed something
    local rounds=$(grep -c '^graph ' $OUT/$1.player.log) agreed=$(grep -c ' 0 disagreements' $OUT/$1.player.log)
    local erased=$(grep -oE 'erased [0-9]+ objects' $OUT/$1.log | awk '{s += $2} END {print s + 0}')
    local refused=$(grep -oE 'refused [0-9]+' $OUT/$1.log | awk '{s += $2} END {print s + 0}')
    [ $rounds -ge 2 ] && [ $rounds = $agreed ] && [ $erased -gt 1000 ] && [ $refused = 0 ]
    check "    $rounds rounds, $agreed agree with the GC, $erased objects freed, $refused refused" 0 $? $OUT/$1.log
}

# Templates, as Microsoft ships them.
for kind in webapp mvc; do
    (cd $OUT && "$DOTNET" new $kind -o $kind --no-https) > $OUT/$kind.new.log 2>&1
    start_app $kind $OUT/$kind
    start_player $kind 300
    served "untouched $kind template" $kind "1000 200" "$(load 1000 /)"
    stop_app
    played $kind
    check "    stops cleanly" 0 $rc $OUT/$kind.log "] exit, used"
done

# Built with an older SDK for an older .NET, run from the dll.
OLD=/usr/local/share/dotnet/dotnet
for v in 8 9; do
    sdk=$($OLD --list-sdks 2> /dev/null | grep "^$v\." | tail -1 | cut -d' ' -f1)
    if [ -z "$sdk" ]; then echo "SKIP  .NET $v app (no SDK)"; continue; fi
    mkdir -p $OUT/net$v && echo "{\"sdk\":{\"version\":\"$sdk\"}}" > $OUT/net$v/global.json
    if (cd $OUT/net$v && $OLD new webapp -n App -o . --no-https --force && echo "{\"sdk\":{\"version\":\"$sdk\"}}" > global.json && $OLD build -c Release) > $OUT/net$v.new.log 2>&1; then
        cp -R $OUT/net$v/wwwroot $OUT/net$v/bin/Release/net$v.0/
        start_app net$v $OUT/net$v/bin/Release/net$v.0/App.dll
        start_player net$v 300
        served "template built for .NET $v" net$v "500 200" "$(load 500 /)"
        stop_app
        grep -q "built for .NET $v, running on .NET 10" $OUT/net$v.log; check "    rolled forward" 0 $? $OUT/net$v.log
        played net$v
    else
        echo "SKIP  .NET $v app (could not be built here: see $OUT/net$v.new.log)"
    fi
done

# A 64 MB limit. Nobody asks for collections: the app freezes when it is full.
start_app limit --limit 64 samples/webapp
start_player limit 100000000
for i in $(seq 200); do curl -s -o /dev/null -X POST $URL/orders/$i; done
served "64 MB limit, 2000 requests making about 0.5 MB each" limit "2000 200" "$(load 2000 /work/{})"
served "    app calling itself over HTTP" limit "50 200" "$(load 50 /self)"
curl -s $URL/orders > $OUT/orders.log; check "    state from before is intact" 0 0 $OUT/orders.log "ok 200"
stop_app
played limit
grep "] exit" $OUT/limit.log | sed 's/^/        /'

if command -v node > /dev/null && [ -x "${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}" ]; then
    start_app button $OUT/webapp
    limit 60 node tests/page7.mjs http://localhost:$WEB_PORT/flight.html $OUT/ > $OUT/page.log 2>&1; rc=$?
    check "the page freezes an app that never collects" 0 $rc $OUT/page.log "ok"
    served "    and it goes on" button "20 200" "$(load 20 /)"
    stop_app
else
    echo "SKIP  page check (needs node and Chrome)"
fi

# The memory limit without a player: the app must fail the way .NET apps do, not hang or corrupt.
start_app noplayer --limit 64 samples/webapp
kill $(pgrep -f "viewer.dll --WebPort $WEB_PORT") 2> /dev/null; sleep 0.5
PLAYER=
got=$(load 400 /work/{}); echo "$got" > $OUT/noplayer.served.log
case "$got" in "400 200") false;; *) true;; esac; check "64 MB limit, no viewer: requests fail once it is full ($got)" 0 $? $OUT/noplayer.served.log
stop_app

# Things that cannot work say so.
mkdir -p $OUT/empty
refuse() { ./play "${@:3}" > $OUT/refuse.log 2>&1; rc=$?; [ $rc != 0 ] && rc=refused; check "$1" refused $rc $OUT/refuse.log "$2"; }
refuse "a folder without a project is refused" "has 0 project files" $OUT/empty
refuse "a missing file is refused" "no such file" $OUT/nothing.dll
touch $OUT/empty/lib.dll; refuse "a library is refused" "no runtimeconfig.json" $OUT/empty/lib.dll
refuse "a native program is refused" "give a .dll" /bin/ls
refuse "a bad limit is refused" "needs a number" --limit lots samples/webapp
pgrep -f "WebPort $WEB_PORT" > $OUT/left.log; [ $? = 1 ]; check "nothing is left running" 0 $? $OUT/left.log
finish "any app"
