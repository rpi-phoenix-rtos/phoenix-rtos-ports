#!/usr/bin/env python3
# Copyright 2026 Phoenix Systems
# SPDX-License-Identifier: CC0-1.0 (the generated image is public domain)
"""labwc-drm: generate the M7 wallpaper, phoenix-gradient-1920x1080.png.

A deterministic, dependency-free (stdlib zlib) 1920x1080 RGB PNG: a diagonal
dark-blue-to-ember gradient with a soft radial glow and faint rings, so a
correct scan-out is recognisable on HDMI (smooth, no banding at 24 bpp) and a
wrong pixel format is obvious (R/B swapped turns the ember blue).
Usage: make-wallpaper.py <out.png>
"""
import math
import struct
import sys
import zlib

W, H = 1920, 1080


def pixel(x, y):
    t = (x / W) * 0.6 + (y / H) * 0.4                       # diagonal 0..1
    r = 16 + t * 200
    g = 24 + t * 70
    b = 64 - t * 40
    dx, dy = (x - W * 0.72) / W, (y - H * 0.68) / H          # glow, lower right
    d = math.hypot(dx, dy * 1.6)
    glow = max(0.0, 1.0 - d * 2.6) ** 2
    r += glow * 255 * 0.55
    g += glow * 150 * 0.55
    b += glow * 40 * 0.55
    ring = 0.5 + 0.5 * math.cos(d * 90.0)                    # faint rings around it
    k = 1.0 + 0.04 * ring * max(0.0, 1.0 - d * 1.5)
    return (min(255, int(r * k)), min(255, int(g * k)), min(255, max(0, int(b * k))))


def main():
    rows = bytearray()
    for y in range(H):
        rows.append(0)                                      # filter: none
        for x in range(W):
            rows.extend(pixel(x, y))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b'')
    with open(sys.argv[1], 'wb') as f:
        f.write(png)


if __name__ == '__main__':
    main()
