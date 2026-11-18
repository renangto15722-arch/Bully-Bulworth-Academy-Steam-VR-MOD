# rtti_vtables.py <dump.exe> <ClassName> [...] - MSVC RTTI -> vtables, in a
# dump whose file offsets equal RVAs (tools\dump-exe.ps1), image base 0x400000.
# Prints every vtable of each class (one per base subobject) and its slots.
import struct
import sys

BASE = 0x400000


def main():
    data = open(sys.argv[1], "rb").read()
    u32 = lambda off: struct.unpack_from("<I", data, off)[0]
    for cls in sys.argv[2:]:
        name = (".?AV%s@@" % cls).encode()
        at = data.find(name + b"\0")
        if at < 0:
            print("%s: no type descriptor" % cls)
            continue
        td = BASE + at - 8
        # complete object locators: signature 0, pTypeDescriptor at +12
        cols = []
        pos = 0
        needle = struct.pack("<I", td)
        while True:
            pos = data.find(needle, pos)
            if pos < 0:
                break
            col = pos - 12
            if col >= 0 and u32(col) == 0:
                cols.append(col)
            pos += 4
        print("%s: type descriptor %08x, %d locator(s)" % (cls, td, len(cols)))
        for col in cols:
            off = u32(col + 4)
            colva = struct.pack("<I", BASE + col)
            p = 0
            while True:
                p = data.find(colva, p)
                if p < 0:
                    break
                vt = p + 4
                slots = []
                for i in range(400):
                    f = u32(vt + i * 4)
                    if not (0x401000 <= f < 0x8fc000):
                        break
                    slots.append(f)
                print("  vtable %08x (this-offset %d), %d slots" % (BASE + vt, off, len(slots)))
                for i in range(0, len(slots), 8):
                    print("    [%2d] %s" % (i, " ".join("%08x" % f for f in slots[i:i + 8])))
                p += 4


main()
