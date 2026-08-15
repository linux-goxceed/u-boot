#!/usr/bin/env python3
"""
mkgxboot.py - build a GX6702 .boot for UART stage-2 upload.

Keeps the vendor stage-1 body (boot[0x20:0x201C]) and the stage-2 metadata
(boot[0x201C:0x203C], including the size word at 0x2038) intact, then replaces
the stage-2 payload at boot[0x203C:] with a padded U-Boot binary.

The vendor stage-1 UART loader validates the stage-2 blob (magic 0x9c0024f0
among other checks).  Replacing the linked gxloader image with raw U-Boot will
always fail with 'E'.  Use gxtest/stage2load.boot + --uboot instead.
"""

import argparse
import struct
import sys

STAGE1_END = 0x201C
STAGE2_META_END = 0x203C
STAGE2_CODE_OFF = 0x203C
STAGE2_LINK = 0x93CE8420
STAGE2_LOAD_BASE = 0x93CE6400


def stage2_payload_cap(vendor: bytes) -> int:
    size = struct.unpack("<I", vendor[0x2038:0x203C])[0]
    if size <= (STAGE2_CODE_OFF - STAGE1_END):
        sys.exit("error: invalid stage-2 size 0x%X at 0x2038" % size)
    return size - (STAGE2_CODE_OFF - STAGE1_END)


def main():
    ap = argparse.ArgumentParser(description="Package U-Boot into a GX6702 .boot")
    ap.add_argument("vendor", help="reference vendor .boot (stage-1 kept)")
    ap.add_argument("uboot", help="U-Boot raw binary (linked at 0x93CE8420)")
    ap.add_argument("out", help="output .boot")
    args = ap.parse_args()

    vendor = bytearray(open(args.vendor, "rb").read())
    uboot = open(args.uboot, "rb").read()

    if vendor[0:4] != b"toob":
        sys.exit("error: vendor file missing 'toob' magic")
    if len(vendor) < STAGE2_CODE_OFF:
        sys.exit("error: vendor boot file too small")

    cap = stage2_payload_cap(vendor)
    if len(uboot) > cap:
        sys.exit("error: U-Boot (%d B) exceeds stage-2 slot (%d B)"
                 % (len(uboot), cap))

    out = vendor[:]
    payload = uboot + bytes(cap - len(uboot))
    out[STAGE2_CODE_OFF:STAGE2_CODE_OFF + cap] = payload
    open(args.out, "wb").write(out)
    meta = struct.unpack("<I", out[0x2038:0x203C])[0]
    print("[mkgxboot] %s: stage-2 meta kept (size 0x%X), U-Boot %d B padded to %d B @ 0x%X (link 0x%08X)"
          % (args.out, meta, len(uboot), cap, STAGE2_CODE_OFF, STAGE2_LINK))


if __name__ == "__main__":
    main()
