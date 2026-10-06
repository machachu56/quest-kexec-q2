#!/usr/bin/env python3
"""Fence off the RAM Android used for dynamically placed reserved regions.

Reserved-memory nodes given as `size` + `alloc-ranges` are placed at boot by
whichever kernel parses the tree. Android hands several of them (secure
display, QSEE TAs, DSP heaps) to the hypervisor or TrustZone, and they stay
locked after kexec. If the target places them elsewhere, Android's old
locations become ordinary free RAM in the target and the first touch hangs
the CPU on the bus. Pinning the pools in place does not work: secure ION
ranges (no-map) lie inside them and reusable CMA hands the rest back to the
allocator. Instead add a no-map node over each Android location and let the
target place fresh pools elsewhere.

Input is Android's boot log lines ("created CMA/DMA memory pool at X" followed
by "initialized node NAME"), as saved by capture.sh.

Usage: fence-reserved.py <dtb> <android-reserved.txt>   (edits dtb in place)
"""
import re
import subprocess
import sys


def fdt(*args):
    return subprocess.run(["fdtget", *args], capture_output=True, text=True)


def cells(dtb, node, prop):
    r = fdt("-t", "x", dtb, node, prop)
    return [int(x, 16) for x in r.stdout.split()] if r.returncode == 0 else None


def main():
    dtb, log = sys.argv[1], sys.argv[2]
    nodes = fdt("-l", dtb, "/reserved-memory").stdout.split()
    addr = None
    pinned = 0
    for line in open(log):
        m = re.search(r"memory pool at (0x[0-9a-f]+)", line)
        if m:
            addr = int(m.group(1), 16)
            continue
        m = re.search(r"initialized node (\S+),", line)
        if not m or addr is None:
            continue
        name, base = m.group(1), addr
        addr = None
        if name not in nodes:
            continue
        size = cells(dtb, f"/reserved-memory/{name}", "size")
        if not size or cells(dtb, f"/reserved-memory/{name}", "reg"):
            continue
        size = (size[0] << 32 | size[1]) if len(size) == 2 else size[0]
        node = f"/reserved-memory/qkx-android-{name.replace(',', '-')}@{base:x}"
        subprocess.run(["fdtput", "-c", dtb, node], check=True)
        subprocess.run(["fdtput", "-t", "x", dtb, node, "reg",
                        f"{base >> 32:x}", f"{base & 0xffffffff:x}",
                        f"{size >> 32:x}", f"{size & 0xffffffff:x}"], check=True)
        subprocess.run(["fdtput", "-t", "s", dtb, node, "no-map", ""], check=True)
        print(f"fenced {name} at {base:#x} size {size:#x}")
        pinned += 1
    print(f"{pinned} Android dynamic regions fenced off (no-map)")


if __name__ == "__main__":
    main()
