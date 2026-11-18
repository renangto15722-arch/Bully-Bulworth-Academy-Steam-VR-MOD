# lua_fn.py <dump.exe> <LuaName> [...] - where a Lua function of the game is
# implemented: finds the name string, then {char* name, lua_CFunction fn}
# pairs (registration tables) pointing at it.
import struct
import sys

BASE = 0x400000
data = open(sys.argv[1], "rb").read()
for name in sys.argv[2:]:
    pos = 0
    found = False
    while True:
        pos = data.find(name.encode() + b"\0", pos)
        if pos < 0:
            break
        if pos == 0 or data[pos - 1] == 0:   # whole string, not a suffix
            ptr = struct.pack("<I", BASE + pos)
            ref = 0
            while True:
                ref = data.find(ptr, ref)
                if ref < 0:
                    break
                after = struct.unpack_from("<I", data, ref + 4)[0]
                before = struct.unpack_from("<I", data, ref - 4)[0]
                print("%s: string %08x, ref at %08x, next dword %08x, previous dword %08x" %
                      (name, BASE + pos, BASE + ref, after, before))
                found = True
                ref += 4
        pos += 1
    if not found:
        print("%s: no reference" % name)
