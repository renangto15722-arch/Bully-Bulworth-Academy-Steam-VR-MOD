# mark_grips.py <shot base path> - draw where the simulator put each controller
# (grip origin) onto the left eye picture of an xrsim-shot capture, to compare
# with where the game's hands came out. Head must be level and facing -Z
# (head rot 0 0 0) - the projection ignores head rotation.
import json
import math
import sys

from PIL import Image, ImageDraw

base = sys.argv[1]
s = json.load(open(base + ".json"))
img = Image.open(base + "_left.png").convert("RGB")
W, H = img.size
eye = s["views"][0]
ex = eye["pos"]
f = eye["fovDeg"]
tl, tr = math.tan(math.radians(f["l"])), math.tan(math.radians(f["r"]))
tu, td = math.tan(math.radians(f["u"])), math.tan(math.radians(f["d"]))
d = ImageDraw.Draw(img)
for key, col in (("handL", (0, 160, 255)), ("handR", (255, 60, 60))):
    h = s.get(key)
    if not h or not h.get("valid"):
        continue
    p = h["grip"]
    z = -(p[2] - ex[2])
    if z <= 0.01:
        continue
    u = ((p[0] - ex[0]) / z - tl) / (tr - tl) * W
    v = (tu - (p[1] - ex[1]) / z) / (tu - td) * H
    d.ellipse((u - 9, v - 9, u + 9, v + 9), outline=col, width=3)
    d.line((u - 16, v, u + 16, v), fill=col, width=2)
    d.line((u, v - 16, u, v + 16), fill=col, width=2)
    print(key, "grip", p, "-> pixel", round(u), round(v))
img.save(base + "_marked.png")
