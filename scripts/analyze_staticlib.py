#!/usr/bin/env python3
"""Report the code vs debug breakdown of a static archive.

Used to keep Rust's target/ and the shipped images lean while porting the
Mocktail kernels: an archive this size is not what ships, only the members the
linker actually references are pulled in, but debug sections that never get
referenced still bloat target/ and get shipped if the archive is copied.
"""
import subprocess
import sys
from collections import defaultdict


def members(path):
    out = subprocess.run(["ar", "t", path], capture_output=True, text=True, check=True)
    return out.stdout.split()


def section_sizes(obj_path):
    out = subprocess.run(
        ["readelf", "-S", "-W", obj_path], capture_output=True, text=True
    ).stdout
    sizes = {}
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("["):
            continue
        # [ 3] .text.unlikely._RINvMs_.. PROGBITS ...
        parts = line.split()
        if len(parts) < 6:
            continue
        name = parts[1]
        try:
            size = int(parts[5], 16)
        except ValueError:
            try:
                size = int(parts[6], 16)
            except (ValueError, IndexError):
                continue
        sizes[name] = size
    return sizes


def main():
    path = sys.argv[1]
    total = defaultdict(int)
    per_member = []
    for m in members(path):
        data = subprocess.run(
            ["ar", "p", path, m], capture_output=True, check=True
        ).stdout
        open("/tmp/.ar_probe.o", "wb").write(data)
        sizes = section_sizes("/tmp/.ar_probe.o")
        code = sum(v for k, v in sizes.items() if k.startswith((".text", ".rodata", ".data", ".data.rel")))
        dbg = sum(v for k, v in sizes.items() if k.startswith(".debug"))
        for k, v in sizes.items():
            total[k] += v
        per_member.append((code, dbg, m))

    code_total = sum(c for c, _, _ in per_member)
    dbg_total = sum(d for _, d, _ in per_member)
    print(f"\n{path}: {len(per_member)} members")
    print(f"  code + rodata + data : {code_total / 1024:10.1f} KiB")
    print(f"  debug sections       : {dbg_total / 1024:10.1f} KiB")
    print(f"  other (symtab/strtab): {(sum(total.values()) - code_total - dbg_total) / 1024:10.1f} KiB")
    print("\n  largest members by (code + debug):")
    for code, dbg, m in sorted(per_member, reverse=True)[:8]:
        crate = m.split(".")[0]
        print(f"    {crate:<28} code {code / 1024:8.1f} KiB  debug {dbg / 1024:8.1f} KiB")


main()
