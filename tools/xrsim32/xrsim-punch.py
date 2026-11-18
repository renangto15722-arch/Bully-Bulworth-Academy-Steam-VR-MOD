# xrsim-punch.py [hand r|l|both] [steps] [reach] - throw a straight punch with a
# simulated controller: the hand goes from the chin (0.15 m in front of the
# head) to `reach` metres in front, in `steps` frames, then back. Each step is
# one "hand <h> offset forward right up" line (relative to the head), written to the sim's command.txt and waited
# for (state.json cmdSeq), so the speed the mod sees is about
# (reach - 0.15) / (steps * frame time).
import json
import os
import sys
import time

d = os.path.join(os.environ["LOCALAPPDATA"], "BullyVR", "xrsim")
state = os.path.join(d, "state.json")
cmd = os.path.join(d, "command.txt")
hand = sys.argv[1] if len(sys.argv) > 1 else "r"
steps = int(sys.argv[2]) if len(sys.argv) > 2 else 6
reach = float(sys.argv[3]) if len(sys.argv) > 3 else 0.75
x = 0.12 if hand == "r" else -0.12


def seq():
    for _ in range(20):
        try:
            return int(json.load(open(state))["cmdSeq"])
        except Exception:
            time.sleep(0.01)
    return -1


def send(line):
    if hand == "both":   # the same stroke with both hands, side by side
        line = line.replace("hand both offset", "hand l offset", 1)
        parts = line.split()
        right = "hand r offset %s %.3f %s" % (parts[3], abs(float(parts[4])), parts[5])
        left = "hand l offset %s %.3f %s" % (parts[3], -abs(float(parts[4])), parts[5])
        line = left + "\n" + right
    before = seq()
    for _ in range(50):
        try:
            with open(cmd, "w") as f:
                f.write(line + "\n")
            break
        except OSError:
            time.sleep(0.005)
    t0 = time.time()
    need = before + line.count("\n") + 1
    while seq() < need and time.time() - t0 < 2:
        time.sleep(0.002)
    s = json.load(open(state))
    if s.get("lastCmdError"):
        print("rejected:", s["lastCmdError"], s.get("lastCmd"))
        sys.exit(1)


send("hand %s offset 0.15 %.3f -0.15" % (hand, x))
time.sleep(0.4)
t = time.time()
for i in range(1, steps + 1):
    f = 0.15 + (reach - 0.15) * i / steps
    send("hand %s offset %.3f %.3f -0.15" % (hand, f, x))
print("out in %.3f s (%.2f m/s)" % (time.time() - t, (reach - 0.15) / max(time.time() - t, 1e-3)))
time.sleep(0.3)
send("hand %s offset 0.35 %.3f -0.3" % (hand, x * 1.6))
