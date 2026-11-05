#!/usr/bin/env python3
"""Print the address ranges in an Intel-HEX file and check expected words.

Usage: python3 hexmap.py file.hex [--expect ADDR=VALUE ...] [--flash-max ADDR]

Prints contiguous data ranges (code space and UICR), then verifies each
--expect word (little-endian 32-bit) and, with --flash-max, that no code-space
data lies at or above that address.  Exit status 1 on any failed check.
"""
import argparse
import struct
import sys


def parse(path):
    data = {}
    ext = 0
    for line in open(path):
        line = line.strip()
        if not line.startswith(":"):
            continue
        b = bytes.fromhex(line[1:])
        n, addr, typ = b[0], (b[1] << 8) | b[2], b[3]
        payload = b[4:4 + n]
        if typ == 4:
            ext = ((payload[0] << 8) | payload[1]) << 16
        elif typ == 2:
            ext = ((payload[0] << 8) | payload[1]) << 4
        elif typ == 0:
            base = ext + addr
            for i, v in enumerate(payload):
                data[base + i] = v
    return data


def ranges(addrs):
    out = []
    start = prev = None
    for a in sorted(addrs):
        if start is None:
            start = prev = a
        elif a == prev + 1:
            prev = a
        else:
            out.append((start, prev + 1))
            start = prev = a
    if start is not None:
        out.append((start, prev + 1))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hex")
    ap.add_argument("--expect", action="append", default=[], help="ADDR=VALUE (hex)")
    ap.add_argument("--flash-max", type=lambda x: int(x, 0), default=None)
    a = ap.parse_args()
    data = parse(a.hex)
    ok = True
    print(f"{a.hex}: {len(data)} bytes")
    for lo, hi in ranges(data.keys()):
        tag = "UICR" if lo >= 0x10001000 else "code"
        print(f"  {tag} {lo:#010x} - {hi:#010x} ({hi - lo} bytes)")
        if tag == "code" and a.flash_max is not None and hi > a.flash_max:
            print(f"    FAIL: extends past {a.flash_max:#x}")
            ok = False
    for e in a.expect:
        addr_s, val_s = e.split("=")
        addr, val = int(addr_s, 0), int(val_s, 0)
        try:
            got = struct.unpack("<I", bytes(data[addr + i] for i in range(4)))[0]
        except KeyError:
            print(f"  FAIL: {addr:#x} not present")
            ok = False
            continue
        status = "ok" if got == val else "FAIL"
        print(f"  {status}: {addr:#x} = {got:#010x} (expected {val:#010x})")
        ok = ok and got == val
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
