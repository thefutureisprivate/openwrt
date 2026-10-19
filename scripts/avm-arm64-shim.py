#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepend an AArch32 Qualcomm execution-switch stub to an arm64 Image."""

import argparse
import pathlib
import struct


def wrap(image, load, limit):
    if len(image) < 64 or image[56:60] != b'ARM\x64':
        raise ValueError('input is not an arm64 Image')
    offset, size, flags = struct.unpack_from('<QQQ', image, 8)
    if not size or size < len(image) or flags & 1:
        raise ValueError('unsupported arm64 Image size or endianness')
    if offset >= 0x200000 or offset & 0xfff or load & 7:
        raise ValueError('unsupported Image offset or unaligned load address')
    entry = ((load + 0x200000) & ~0x1fffff) + offset
    if not 0 <= load < entry or entry + size > limit or limit > 0x100000000:
        raise ValueError('Image overlaps the reserved-memory limit')

    # EVA enters ARM state with r2 pointing to its patched DTB. The SiP
    # execution-switch call takes an 80-byte array: x0..x8, then the entry PC.
    # TrustZone 7.14.1.1.0 restores this context at nonsecure AArch64 EL1h.
    # Emit ARM instructions directly to avoid requiring an ARM32 toolchain
    # when building an ARM64 target. The parameter block starts at +0x80.
    code = (
        0xf10c00c0,  # cpsid if
        0xee110f10,  # mrc p15, 0, r0, c1, c0, 0 (SCTLR)
        0xe3100005,  # tst r0, #5 (MMU and data cache must be off)
        0x1a00000d,  # bne .hang (+0x48)
        0xe3120007,  # tst r2, #7 (DTB must be 8-byte aligned)
        0x1a00000b,  # bne .hang
        0xe3520000,  # cmp r2, #0
        0x0a000009,  # beq .hang
        0xe28f3058,  # adr r3, parameters (+0x80)
        0xe5832000,  # str r2, [r3] (x0; all high words are initially zero)
        0xe1a02003,  # mov r2, r3
        0xe3a01011,  # mov r1, #0x11 (one read-only buffer argument)
        0xe59f0018,  # ldr r0, function_id (+0x50)
        0xe3a03050,  # mov r3, #80
        0xf57ff04f,  # dsb sy
        0xf57ff06f,  # isb
        0xe1600070,  # smc #0
        0xeaffffff,  # b .hang (an unsupported SMC must not enter the Image)
        0xeafffffe,  # .hang: b .hang
        0xe320f000,  # nop
        0x8200010f,  # Qualcomm SiP execution switch
    )
    stub = struct.pack('<%dI' % len(code), *code).ljust(0x80, b'\0')
    stub += struct.pack('<10Q', *([0] * 9 + [entry]))
    return stub.ljust(entry - load, b'\0') + image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--load', type=lambda s: int(s, 0), required=True)
    parser.add_argument('--limit', type=lambda s: int(s, 0), required=True)
    args = parser.parse_args()
    try:
        result = wrap(args.input.read_bytes(), args.load, args.limit)
    except (OSError, ValueError) as err:
        parser.error(str(err))
    args.output.write_bytes(result)


if __name__ == '__main__':
    main()
