"""Add an import of <dll>!<func> to a PE32 exe (new section holding a copy of the import table).
usage: inject_import.py in.exe out.exe afdkfix.dll Init
"""
import struct, sys

src, dst, dll, func = sys.argv[1:5]
d = bytearray(open(src, "rb").read())

e = struct.unpack_from("<I", d, 0x3C)[0]
assert d[e:e+4] == b"PE\0\0"
nsec = struct.unpack_from("<H", d, e + 6)[0]
optsz = struct.unpack_from("<H", d, e + 20)[0]
opt = e + 24
assert struct.unpack_from("<H", d, opt)[0] == 0x10B
sect_align, file_align = struct.unpack_from("<II", d, opt + 32)
size_headers = struct.unpack_from("<I", d, opt + 60)[0]
dd = opt + 96                       # data directories
imp_rva, imp_size = struct.unpack_from("<II", d, dd + 8)
st = opt + optsz                    # section table
secs = []
for i in range(nsec):
    o = st + 40 * i
    name = d[o:o+8].rstrip(b"\0").decode()
    vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
    secs.append((name, vsz, va, rsz, rptr))
assert st + 40 * (nsec + 1) <= secs[0][4], "no room for another section header"

def rva2off(rva):
    for n, vsz, va, rsz, rptr in secs:
        if va <= rva < va + max(vsz, rsz):
            return rptr + rva - va
    raise ValueError(hex(rva))

# existing descriptors
descs = []
o = rva2off(imp_rva)
while True:
    x = d[o:o+20]
    if x == b"\0" * 20: break
    descs.append(bytes(x)); o += 20

def align(x, a): return (x + a - 1) // a * a

last = secs[-1]
new_va = align(last[2] + max(last[1], last[3]), sect_align)
new_off = align(len(d), file_align)

n_desc = len(descs) + 2
blob = bytearray()
def here(): return new_va + len(blob)

# layout: descriptors | ilt | iat | hint/name | dllname
desc_len = n_desc * 20
ilt_off = desc_len
iat_off = ilt_off + 8
hn_off = iat_off + 8
fname = func.encode() + b"\0"
hn = struct.pack("<H", 0) + fname
if len(hn) % 2: hn += b"\0"
dll_off = hn_off + len(hn)
total = dll_off + len(dll) + 1

blob = bytearray(total)
for i, x in enumerate(descs):
    blob[i*20:(i+1)*20] = x
nd = len(descs)
# new descriptor: OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk
struct.pack_into("<IIIII", blob, nd*20, new_va + ilt_off, 0, 0, new_va + dll_off, new_va + iat_off)
struct.pack_into("<I", blob, ilt_off, new_va + hn_off)
struct.pack_into("<I", blob, iat_off, new_va + hn_off)
blob[hn_off:hn_off+len(hn)] = hn
blob[dll_off:dll_off+len(dll)+1] = dll.encode() + b"\0"

raw = align(len(blob), file_align)
d += b"\0" * (new_off - len(d))
d += blob + b"\0" * (raw - len(blob))

# section header
o = st + 40 * nsec
d[o:o+40] = b".afx\0\0\0\0" + struct.pack("<IIIIIIHHI", len(blob), new_va, raw, new_off, 0, 0, 0, 0, 0xC0000040)
struct.pack_into("<H", d, e + 6, nsec + 1)
struct.pack_into("<I", d, opt + 56, align(new_va + len(blob), sect_align))   # SizeOfImage
struct.pack_into("<II", d, dd + 8, new_va, desc_len)                          # import dir
struct.pack_into("<II", d, dd + 11 * 8, 0, 0)                                 # drop bound imports
struct.pack_into("<I", d, opt + 64, 0)                                        # checksum
open(dst, "wb").write(d)
print("ok: new section at RVA %#x, file offset %#x, %d descriptors" % (new_va, new_off, n_desc - 1))
