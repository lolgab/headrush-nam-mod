import struct, sys

path = sys.argv[1]

with open(path, "rb") as f:
    data = f.read()

magic, totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap, version, last_comp_version, boot_cpuid_phys, size_dt_strings, size_dt_struct = struct.unpack(">10I", data[0:40])
assert magic == 0xd00dfeed, hex(magic)

FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9

def get_string(off):
    end = data.index(b'\0', off_dt_strings + off)
    return data[off_dt_strings+off:end].decode()

off = off_dt_struct
stack = []
results = {}

while off < off_dt_struct + size_dt_struct:
    tok = struct.unpack(">I", data[off:off+4])[0]
    off += 4
    if tok == FDT_BEGIN_NODE:
        end = data.index(b'\0', off)
        name = data[off:end].decode()
        off = end + 1
        off = (off + 3) & ~3
        stack.append(name)
    elif tok == FDT_END_NODE:
        stack.pop()
    elif tok == FDT_PROP:
        plen, nameoff = struct.unpack(">II", data[off:off+8])
        off += 8
        prop_name = get_string(nameoff)
        data_off = off
        curpath = "/" + "/".join(stack)
        results[(curpath, prop_name)] = (data_off, plen)
        off += plen
        off = (off + 3) & ~3
    elif tok == FDT_NOP:
        pass
    elif tok == FDT_END:
        break
    else:
        raise Exception(f"unknown token {tok} at {off}")

for k, v in results.items():
    if k[1] in ("data", "description", "compression", "type", "os", "arch", "load", "entry"):
        print(k, "-> file_offset=%d len=%d" % v)
