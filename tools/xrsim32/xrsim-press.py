# xrsim-press.py <btn> [hold_s] [then_wait_s] [<btn> [hold_s] [wait_s] ...] - tap
# simulated controller buttons in sequence (a, b, x, y, menu ...), each held
# hold_s seconds and followed by wait_s seconds, with one process (no
# PowerShell start-up between presses).
import json
import os
import sys
import time

d = os.path.join(os.environ["LOCALAPPDATA"], "BullyVR", "xrsim")
state = os.path.join(d, "state.json")
cmd = os.path.join(d, "command.txt")


def seq():
    for _ in range(20):
        try:
            return int(json.load(open(state))["cmdSeq"])
        except Exception:
            time.sleep(0.01)
    return -1


def send(line):
    before = seq()
    for _ in range(50):
        try:
            with open(cmd, "w") as f:
                f.write(line + "\n")
            break
        except OSError:
            time.sleep(0.005)
    t0 = time.time()
    while seq() <= before and time.time() - t0 < 2:
        time.sleep(0.002)
    s = json.load(open(state))
    if s.get("lastCmdError"):
        print("rejected:", s["lastCmdError"], s.get("lastCmd"))
        sys.exit(1)


args = sys.argv[1:]
i = 0
while i < len(args):
    btn = args[i]
    hold = float(args[i + 1]) if i + 1 < len(args) else 0.2
    wait = float(args[i + 2]) if i + 2 < len(args) else 1.0
    i += 3
    send("btn %s down" % btn)
    time.sleep(hold)
    send("btn %s up" % btn)
    print("%s pressed %.2f s" % (btn, hold), flush=True)
    time.sleep(wait)
