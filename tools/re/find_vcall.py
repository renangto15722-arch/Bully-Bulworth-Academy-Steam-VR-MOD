# find_vcall.py <dump.exe> <offset hex> - every "call [reg+offset]" (FF /2
# with disp32) in .text of a dump whose file offsets equal RVAs.
import struct
import sys

BASE = 0x400000
data = open(sys.argv[1], "rb").read()
disp = struct.pack("<I", int(sys.argv[2], 16))
text_end = 0x1000 + 0x4fa992
pos = 0x1000
while True:
    pos = data.find(disp, pos, text_end)
    if pos < 0:
        break
    op, modrm = data[pos - 2], data[pos - 1]
    # FF /2 mod=10 (disp32), rm != 100 (no SIB)
    if (modrm & 0xC0) == 0x80 and (modrm & 7) != 4 and op in (0xFF, 0x8B):
        print("%08x %s r%d,[%s+%s]" % (BASE + pos - 2, "call" if op == 0xFF else "mov", (modrm >> 3) & 7, ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"][modrm & 7], sys.argv[2]))
    pos += 1
