# find_ptrs.py <dump.exe> <lo hex> <hi hex> - dwords in .rdata/.data that point
# into [lo, hi): vtable slots / callbacks aimed at code with no known start.
import struct
import sys

BASE = 0x400000
data = open(sys.argv[1], "rb").read()
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
for off in range(0x4fc000, 0x4fc000 + 0xc7bca + 0x400000, 4):
    if off + 4 > len(data):
        break
    v = struct.unpack_from("<I", data, off)[0]
    if lo <= v < hi:
        print("%08x -> %08x" % (BASE + off, v))
