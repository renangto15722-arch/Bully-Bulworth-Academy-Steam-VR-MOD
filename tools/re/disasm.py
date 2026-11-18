# disasm.py <dump.exe> <addr hex> [count] - raw bytes of the dump at an
# address (file offset = RVA), 16 per line, to read instructions by hand
# when Ghidra's listing is not at hand.
import sys

BASE = 0x400000
data = open(sys.argv[1], "rb").read()
addr = int(sys.argv[2], 16)
count = int(sys.argv[3]) if len(sys.argv) > 3 else 64
off = addr - BASE
for i in range(0, count, 16):
    chunk = data[off + i: off + i + 16]
    print("%08x  %s" % (addr + i, " ".join("%02x" % b for b in chunk)))
