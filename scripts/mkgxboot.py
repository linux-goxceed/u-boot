#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Build a vendor-free GX6702 UART .boot image from the open IPL and U-Boot.

The first 0x2020 bytes are the open IPL's normal ``toob`` container.  A small
``GXUB`` header and raw U-Boot follow it.  A normal GX uploader first sends the
fixed 8 KiB IPL window, then sends the transformed complete file after the IPL
requests its second stage.  Bundle-aware open IPL versions validate ``GXUB``,
move U-Boot to 0x93ce8420, and enter it.

This is a UART loader image, not a flashable BOOT partition image.
"""

import argparse
import struct
from pathlib import Path

TOOB_SIZE = 0x2020
IPL_BODY_SIZE = 0x2000
IPL_CONFIG_OFF = 0x20 + 0x1E00
IPL_CONFIG_SIZE = 512
IPL_CONFIG_MAGIC = 0x47464331
UBOOT_BUNDLE_MAGIC = 0x42555847
STAGE2_LINK = 0x93CE8420
UBOOT_MAX_SIZE = 16 * 1024 * 1024


def checksum32(payload: bytes) -> int:
    return sum(payload) & 0xFFFFFFFF


def validate_ipl(parser: argparse.ArgumentParser, image: bytes) -> None:
    if len(image) != TOOB_SIZE:
        parser.error(
            f"open IPL must be exactly {TOOB_SIZE} bytes, got {len(image)}"
        )
    if image[:4] != b"toob":
        parser.error("open IPL is missing the 'toob' container magic")
    if struct.unpack_from("<H", image, 6)[0] != 0x6701:
        parser.error("open IPL is not a GX6702/Gemini (chip 0x6701) image")
    config = image[IPL_CONFIG_OFF:IPL_CONFIG_OFF + IPL_CONFIG_SIZE]
    if struct.unpack_from("<I", config)[0] != IPL_CONFIG_MAGIC:
        parser.error("input has no open-IPL config; refusing a vendor loader")
    stored_crc = struct.unpack_from("<H", config, 6)[0]
    config_crc = sum(
        config[index]
        for index in range(8, IPL_CONFIG_SIZE)
        if not 0x1F8 <= index < 0x1FC
    ) & 0xFFFF
    if stored_crc != config_crc:
        parser.error(
            f"open-IPL config checksum is invalid "
            f"(stored 0x{stored_crc:04X}, expected 0x{config_crc:04X})"
        )
    if b"GXUB" not in image[0x20:0x20 + IPL_BODY_SIZE]:
        parser.error(
            "open IPL lacks GXUB bundle support; rebuild the current gxipl tree"
        )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Package the open GX6702 IPL and U-Boot into one UART .boot"
    )
    ap.add_argument("ipl", type=Path, help="bundle-aware gx6702-ipl.boot")
    ap.add_argument("uboot", type=Path,
                    help="raw U-Boot binary linked at 0x93CE8420")
    ap.add_argument("out", type=Path, help="output UART .boot image")
    args = ap.parse_args()

    if args.out.resolve() in (args.ipl.resolve(), args.uboot.resolve()):
        ap.error("output must not overwrite the IPL or U-Boot input")

    ipl = args.ipl.read_bytes()
    uboot = args.uboot.read_bytes()
    validate_ipl(ap, ipl)
    if not uboot:
        ap.error("U-Boot image is empty")
    if len(uboot) > UBOOT_MAX_SIZE:
        ap.error(
            f"U-Boot is {len(uboot)} bytes; open IPL limit is {UBOOT_MAX_SIZE}"
        )

    uboot_checksum = checksum32(uboot)
    header = struct.pack(
        "<IIII",
        UBOOT_BUNDLE_MAGIC,
        len(uboot),
        STAGE2_LINK,
        uboot_checksum,
    )
    output = ipl + header + uboot
    args.out.write_bytes(output)
    print(
        f"[mkgxboot] {args.out}: open IPL {len(ipl)} B + GXUB header + "
        f"U-Boot {len(uboot)} B @ 0x{STAGE2_LINK:08X} "
        f"(checksum 0x{uboot_checksum:08X}, total {len(output)} B)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
