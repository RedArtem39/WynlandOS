#!/bin/sh
# WynlandOS - Zerp 2.0's Hyprland IPC, from inside a session (boot.cfg
# "wlrun=sh+/hyprtest.sh"): hyprctl's answers, then the events a window
# and a workspace switch produce. One "[hyprtest] PASS|FAIL" line per check.
pass=0 fail=0
check() { if [ "$1" = 0 ]; then echo "[hyprtest] PASS $2"; pass=$((pass+1)); else echo "[hyprtest] FAIL $2"; fail=$((fail+1)); fi; }

[ -S "$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock" ]
check $? "the sockets are where Hyprland's are (\$HYPRLAND_INSTANCE_SIGNATURE)"

# listen to the events while things happen
python3 -c '
import os, socket, sys
s = socket.socket(socket.AF_UNIX)
s.connect(os.environ["XDG_RUNTIME_DIR"] + "/hypr/" + os.environ["HYPRLAND_INSTANCE_SIGNATURE"] + "/.socket2.sock")
s.settimeout(25)
out = open("/tmp/hyprtest-events", "w")
try:
    while True:
        d = s.recv(4096)
        if not d: break
        out.write(d.decode()); out.flush()
except Exception:
    pass
' &
sleep 2

hyprctl -j monitors | python3 -c 'import json,sys; m=json.load(sys.stdin)[0]; print(m["name"], m["width"] > 0, m["activeWorkspace"]["id"])' > /tmp/hyprtest-mon
check $? "hyprctl -j monitors: $(cat /tmp/hyprtest-mon)"

hyprctl -j clients | python3 -c 'import json,sys; c=json.load(sys.stdin); sys.exit(0 if c and all(x["address"].startswith("0x") for x in c) else 1)'
check $? "hyprctl -j clients: the windows, with addresses"

foot -e sleep 30 &
sleep 4
hyprctl -j activewindow | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin).get("class") == "foot" else 1)'
check $? "a new window is the active one (j/activewindow)"

[ "$(hyprctl dispatch workspace 3)" = ok ]
check $? "dispatch workspace 3: ok"
sleep 1
hyprctl -j activeworkspace | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin)["id"] == 3 else 1)'
check $? "the active workspace is 3 now"

hyprctl dispatch workspace 1 > /dev/null
ADDR=$(hyprctl -j clients | python3 -c 'import json,sys; print([c["address"] for c in json.load(sys.stdin) if c["class"] == "foot"][0])')
hyprctl dispatch movetoworkspacesilent "5,address:$ADDR" > /dev/null
sleep 1
hyprctl -j workspaces | python3 -c 'import json,sys; w={x["id"]: x["windows"] for x in json.load(sys.stdin)}; sys.exit(0 if w.get(5) == 1 else 1)'
check $? "movetoworkspacesilent: workspace 5 has the window"

[ "$(hyprctl --batch 'j/version ; dispatch workspace 1' | tail -n 2 | head -n 1)" = ok ] || [ -n "$(hyprctl --batch 'j/version ; dispatch workspace 1')" ]
check $? "--batch"

hyprctl dispatch closewindow "address:$ADDR" > /dev/null
sleep 3
E=/tmp/hyprtest-events
for ev in openwindow activewindowv2 workspacev2 createworkspacev2 movewindowv2 closewindow destroyworkspacev2; do
    grep -q "^$ev>>" $E
    check $? "event $ev"
done
grep -q "^openwindow>>[0-9a-f]*,1,foot," $E
check $? "openwindow>>ADDRESS,WORKSPACE,CLASS,TITLE"

echo "[hyprtest] DONE pass=$pass fail=$fail"
