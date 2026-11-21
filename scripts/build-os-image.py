#!/usr/bin/env python3
"""Build the MacDeck OS boot image for the SM-T210.

Takes Samsung's stock kernel and a few stock ramdisk files from the backed-up KERNEL.img,
adds our init.rc / default.prop / deck binary, and packs an Android v0 boot image with the
same load addresses as the stock one. Output: build/macdeck-os-v0.img (+ .sha256).

Usage: scripts/build-os-image.py [path/to/deck0-binary]
Env:   MACDECK_STOCK_BOOT=path/to/stock/KERNEL.img (a raw dump of the tablet's KERNEL
       partition, e.g. made with scripts/backup-partitions.sh). Samsung's binaries are read
       from it at build time and are never stored in this repository.
"""
import gzip, hashlib, io, os, stat, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STOCK = os.environ.get("MACDECK_STOCK_BOOT",
                       os.path.join(ROOT, "backups/2026-11-21-stock-with-twrp/KERNEL.img"))
OUT = os.path.join(ROOT, "build/macdeck-os-v0.img")
DECK = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/deck0")
PAGE = 2048
KERNEL_ADDR, RAMDISK_ADDR, TAGS_ADDR = 0x10008000, 0x11000000, 0x10000100
CMDLINE = b"androidboot.selinux=permissive"
PARTITION_LIMIT = 12 * 1024 * 1024  # RECOVERY / KERNEL are 12 MiB

# Stock files we reuse (Samsung binaries/policy; extracted at build time, never committed).
STOCK_FILES = ["init", "sbin/adbd", "sepolicy", "file_contexts", "property_contexts",
               "seapp_contexts", "ueventd.rc", "ueventd.pxa988.rc"]


def read_stock():
    d = open(STOCK, "rb").read()
    assert d[:8] == b"ANDROID!", "stock KERNEL.img is not a boot image"
    ks, ka, rs, ra = struct.unpack("<4I", d[8:24])
    koff = PAGE
    roff = PAGE + -(-ks // PAGE) * PAGE
    return d[koff:koff + ks], gzip.decompress(d[roff:roff + rs])


def parse_newc(blob):
    files, off = {}, 0
    while True:
        h = blob[off:off + 110]; assert h[:6] == b"070701"
        f = [int(h[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        mode, namesize, filesize = f[1], f[11], f[6]
        noff = off + 110; name = blob[noff:noff + namesize - 1].decode()
        doff = (noff + namesize + 3) & ~3
        data = blob[doff:doff + filesize]
        off = (doff + filesize + 3) & ~3
        if name == "TRAILER!!!": return files
        files[name] = (mode, data)


def newc(entries):
    out, ino = io.BytesIO(), 300000
    for name, mode, data in entries + [("TRAILER!!!", 0, b"")]:
        ino += 1
        nb = name.encode() + b"\0"
        hdr = "070701" + "".join("%08X" % v for v in (
            ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nb), 0))
        out.write(hdr.encode() + nb)
        out.write(b"\0" * (-out.tell() % 4)); out.write(data); out.write(b"\0" * (-out.tell() % 4))
    return out.getvalue()


def main():
    kernel, stock_rd = read_stock()
    stock = parse_newc(stock_rd)
    D, F, X, L = stat.S_IFDIR, stat.S_IFREG, 0o750, stat.S_IFLNK
    entries = [(d, D | 0o755, b"") for d in ("dev", "proc", "sys", "system", "tmp", "data", "mnt", "sbin")]
    for name in STOCK_FILES:
        mode, data = stock[name]
        entries.append((name, mode, data))
    entries.append(("sbin/ueventd", L | 0o777, b"../init"))
    for name, src, perm in (("init.rc", "os/ramdisk/init.rc", 0o750),
                            ("default.prop", "os/ramdisk/default.prop", 0o644)):
        entries.append((name, F | perm, open(os.path.join(ROOT, src), "rb").read()))
    entries.append(("sbin/deck0", F | X, open(DECK, "rb").read()))

    ramdisk = gzip.compress(newc(entries), compresslevel=9, mtime=0)
    hdr = struct.pack("<8s10I16s512s32s", b"ANDROID!", len(kernel), KERNEL_ADDR, len(ramdisk),
                      RAMDISK_ADDR, 0, 0x10f00000, TAGS_ADDR, PAGE, 0, 0, b"", CMDLINE,
                      hashlib.sha1(kernel + struct.pack("<I", len(kernel)) + ramdisk +
                                   struct.pack("<I", len(ramdisk)) + struct.pack("<I", 0)).digest())
    pad = lambda b: b + b"\0" * (-len(b) % PAGE)
    img = pad(hdr) + pad(kernel) + pad(ramdisk)
    assert len(img) <= PARTITION_LIMIT, "image too large for the 12 MiB partition"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "wb").write(img)
    sha = hashlib.sha256(img).hexdigest()
    open(OUT + ".sha256", "w").write(f"{sha}  {os.path.basename(OUT)}\n")
    print(f"{OUT}: {len(img)} bytes, kernel {len(kernel)}, ramdisk {len(ramdisk)} ({len(entries)} entries)")
    print(f"sha256 {sha}")


if __name__ == "__main__":
    main()
