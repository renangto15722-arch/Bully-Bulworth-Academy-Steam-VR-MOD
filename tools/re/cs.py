# cs.py <dump.exe> <addr hex> [bytes] - x86 disassembly of the dump (capstone).
import sys
import capstone

BASE = 0x400000
data = open(sys.argv[1], "rb").read()
addr = int(sys.argv[2], 16)
count = int(sys.argv[3], 0) if len(sys.argv) > 3 else 128
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for ins in md.disasm(data[addr - BASE: addr - BASE + count], addr):
    print("%08x  %-24s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
