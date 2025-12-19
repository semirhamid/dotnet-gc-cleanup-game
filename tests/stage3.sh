#!/bin/bash
# Stage 3 "done when": GC.Collect() in a sample shows the heap in the viewer.
set -u
cd "$(dirname "$0")/.."
OUT=artifacts/stage3
. tests/common.sh
build orchard
build_project viewer/viewer.csproj viewer
build_project tests/player/player.csproj player
export DOTNET_GCName=libmanualgc.dylib
ORCHARD=$(sample orchard)

limit 60 "$DOTNET" $ORCHARD 3 > $OUT/noviewer.log 2>&1; rc=$?
check "no viewer: GC.Collect() returns and the app finishes" 0 $rc $OUT/noviewer.log "resumed"

unset DOTNET_GCName
start_viewer
player look > $OUT/look.log 2>&1 & PLAYER=$!
await connected $OUT/look.log
DOTNET_GCName=libmanualgc.dylib limit 60 "$DOTNET" $ORCHARD 1 > $OUT/orchard.log 2>&1; rc=$?
check "app resumes after the snapshot" 0 $rc $OUT/orchard.log "resumed"
wait $PLAYER; rc=$?
for want in "Apple: live 100 dead 101" "Basket<Apple, int>: live 0 dead 50" "Basket<string, Tree.Leaf>: live 20 dead 0" \
            "Tree.Leaf[]: live 10 dead 0" "Apple[][]: live 0 dead 10" "Pear[,]: live 0 dead 5" "byte[]: live 0 dead 200" \
            "System.Collections.Generic.Dictionary<Apple, System.Collections.Generic.List<Pear>>: live 0 dead 1" \
            "tiled: True" "unnamed types: 0 of"; do
    check "snapshot has $want" 0 $rc $OUT/look.log "$want"
done

code=$(curl -s -o /dev/null -w '%{http_code}' -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' \
    -H 'Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==' -H 'Origin: http://evil.example' "http://localhost:$WEB_PORT/ws")
echo "$code" > $OUT/origin.log
check "a page from another site cannot connect" 0 0 $OUT/origin.log 403
curl -s -o /dev/null -w '%{http_code}' -H 'Host: evil.example' "http://127.0.0.1:$WEB_PORT/" > $OUT/host.log
check "another host name is rejected" 0 0 $OUT/host.log 400

if command -v node > /dev/null && [ -x "${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}" ]; then
    limit 60 node tests/page3.mjs http://localhost:$WEB_PORT/ $OUT/page.png > $OUT/page.log 2>&1; rc=$?
    check "page draws the heap without script errors ($OUT/page.png)" 0 $rc $OUT/page.log "ok"
else
    echo "SKIP  page check (needs node and Chrome)"
fi
stop_viewer
finish "stage 3"
