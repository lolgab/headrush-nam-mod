"""
fit_image.py -- minimal FDT/FIT (U-Boot Flattened Image Tree) reader, reusable
as a module. Used by build_update_img.py to pull the splash/recoverysplash/
rootfs blobs and root metadata (description/compatible/inmusic,devices) out
of a HeadRush Update.img, and to regenerate a matching .its source for
`mkimage` when repacking a modified rootfs back in.

Only implements what HeadRush's Update.img actually uses: FDT_BEGIN_NODE/
FDT_END_NODE/FDT_PROP/FDT_NOP/FDT_END tokens, no phandles/aliases.
"""
import struct

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9


def parse_fdt(data):
    """Returns {(node_path, prop_name): (data_file_offset, prop_len)}."""
    (magic, totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap,
     version, last_comp_version, boot_cpuid_phys, size_dt_strings,
     size_dt_struct) = struct.unpack(">10I", data[0:40])
    assert magic == FDT_MAGIC, f"not a FIT image (bad FDT magic 0x{magic:x})"

    def get_string(off):
        end = data.index(b"\0", off_dt_strings + off)
        return data[off_dt_strings + off:end].decode()

    off = off_dt_struct
    stack = []
    results = {}
    while off < off_dt_struct + size_dt_struct:
        tok = struct.unpack(">I", data[off:off + 4])[0]
        off += 4
        if tok == FDT_BEGIN_NODE:
            end = data.index(b"\0", off)
            name = data[off:end].decode()
            off = (end + 1 + 3) & ~3
            stack.append(name)
        elif tok == FDT_END_NODE:
            stack.pop()
        elif tok == FDT_PROP:
            plen, nameoff = struct.unpack(">II", data[off:off + 8])
            off += 8
            prop_name = get_string(nameoff)
            data_off = off
            curpath = "/" + "/".join(stack)
            results[(curpath, prop_name)] = (data_off, plen)
            off = (off + plen + 3) & ~3
        elif tok == FDT_NOP:
            pass
        elif tok == FDT_END:
            break
        else:
            raise ValueError(f"unknown FDT token {tok} at offset {off}")
    return results


def read_prop_bytes(data, props, path, name):
    off, length = props[(path, name)]
    return data[off:off + length]


def read_prop_str(data, props, path, name):
    raw = read_prop_bytes(data, props, path, name)
    return raw.rstrip(b"\x00").decode()


def read_prop_cells(data, props, path, name):
    raw = read_prop_bytes(data, props, path, name)
    assert len(raw) % 4 == 0
    return list(struct.unpack(f">{len(raw)//4}I", raw))


def read_root_metadata(data, props):
    """Root-node description/compatible/inmusic,devices, as found on every
    HeadRush Update.img seen so far. Raises KeyError with a clear message if
    a future firmware version restructures the root node."""
    return {
        "description": read_prop_str(data, props, "/", "description"),
        "compatible": read_prop_str(data, props, "/", "compatible"),
        "devices": read_prop_cells(data, props, "/", "inmusic,devices"),
    }


def build_its(metadata, splash_xz, recoverysplash_xz, rootfs_xz):
    devices_str = " ".join(f"0x{d:08x}" for d in metadata["devices"])
    return f"""/dts-v1/;

/ {{
\tdescription = "{metadata['description']}";
\tcompatible = "{metadata['compatible']}";
\tinmusic,devices = <{devices_str}>;

\timages {{
\t\tsplash {{
\t\t\tdescription = "Splash screen";
\t\t\tpartition = "splash";
\t\t\tdata = /incbin/("{splash_xz}");
\t\t\tcompression = "xz";
\t\t\thash {{
\t\t\t\talgo = "sha1";
\t\t\t}};
\t\t}};

\t\trecoverysplash {{
\t\t\tdescription = "Update mode splash screen";
\t\t\tpartition = "recoverysplash";
\t\t\tdata = /incbin/("{recoverysplash_xz}");
\t\t\tcompression = "xz";
\t\t\thash {{
\t\t\t\talgo = "sha1";
\t\t\t}};
\t\t}};

\t\trootfs {{
\t\t\tdescription = "Root filesystem";
\t\t\tpartition = "rootfs";
\t\t\tdata = /incbin/("{rootfs_xz}");
\t\t\tcompression = "xz";
\t\t\thash {{
\t\t\t\talgo = "sha1";
\t\t\t}};
\t\t}};
\t}};
}};
"""
