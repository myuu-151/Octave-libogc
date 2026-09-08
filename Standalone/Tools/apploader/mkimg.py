#!/usr/bin/env python3
# Stamp the apploader header onto the raw body binary to produce a valid
# GameCube apploader.img:
#
#   0x00  date string (16 bytes)
#   0x10  entry point   (u32, big-endian) = 0x81200000
#   0x14  body size     (u32) bytes loaded to 0x81200000
#   0x18  trailer size  (u32) BSS bytes the IPL reserves after the body
#   0x1C  padding
#   0x20  body...
import sys, struct

if len(sys.argv) < 3:
    print("usage: mkimg.py <body.bin> <out.img> [bss_size]")
    sys.exit(1)

body = open(sys.argv[1], "rb").read()
out_path = sys.argv[2]
bss = int(sys.argv[3]) if len(sys.argv) > 3 else 0

# Everything the IPL DMAs must be 32-byte aligned.
if len(body) % 32:
    body += b"\x00" * (32 - (len(body) % 32))
bss = (bss + 31) & ~31

hdr = bytearray(0x20)
date = b"2024/01/01 00:00"
hdr[0:len(date)] = date
struct.pack_into(">I", hdr, 0x10, 0x81200000)   # entry
struct.pack_into(">I", hdr, 0x14, len(body))    # body size
struct.pack_into(">I", hdr, 0x18, bss)          # trailer (bss)
struct.pack_into(">I", hdr, 0x1C, 0)

with open(out_path, "wb") as f:
    f.write(bytes(hdr))
    f.write(body)

print("wrote %s  (body=%d bytes, bss=%d bytes)" % (out_path, len(body), bss))
