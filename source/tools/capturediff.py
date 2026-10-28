#!/usr/bin/env python3
"""Compares two folders of visual parity captures (made with --capture).

For each PNG in both folders, prints the mean difference per channel and the share
of pixels where any channel differs by more than --threshold, and writes an
amplified difference image to --out for every capture over --tolerance. Exits 1
when any capture is over tolerance or missing.

Usage: capturediff.py <reference folder> <new folder> [--out folder]
       [--threshold 16] [--tolerance 1.0]
"""
import argparse
import os
import struct
import sys
import zlib


def read_png(path):
    """Returns (width, height, channels, rows) for an 8-bit RGB or RGBA PNG."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path} is not a PNG')
    pos, idat = 8, []
    width = height = channels = None
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b'IHDR':
            width, height, depth, color, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or color not in (2, 6) or interlace:
                raise ValueError(f'{path}: only 8-bit, non-interlaced RGB or RGBA PNGs are supported')
            channels = 3 if color == 2 else 4
        elif kind == b'IDAT':
            idat.append(body)
        elif kind == b'IEND':
            break

    raw = zlib.decompress(b''.join(idat))
    stride = width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        row = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = prev[x]
            if kind == 1:
                row[x] = (row[x] + left) & 255
            elif kind == 2:
                row[x] = (row[x] + up) & 255
            elif kind == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 255
            elif kind == 4:
                corner = prev[x - channels] if x >= channels else 0
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                pred = left if pa <= pb and pa <= pc else (up if pb <= pc else corner)
                row[x] = (row[x] + pred) & 255
        rows.append(row)
        prev = row
    return width, height, channels, rows


def write_png(path, width, height, rows):
    """Writes 8-bit RGB rows."""
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''.join(b'\x00' + bytes(row) for row in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


def compare(reference, new, threshold):
    """Returns (mean difference, share of pixels over threshold, difference rows)."""
    w, h, ca, rows_a = reference
    w2, h2, cb, rows_b = new
    if (w, h) != (w2, h2):
        raise ValueError(f'sizes differ: {w}x{h} and {w2}x{h2}')
    total = over = 0
    diff_rows = []
    blank = bytearray(w * 3)
    for row_a, row_b in zip(rows_a, rows_b):
        #Most rows match exactly between close captures, so skip them quickly
        if ca == cb and row_a == row_b:
            diff_rows.append(blank)
            continue
        diff = bytearray(w * 3)
        for x in range(w):
            worst = 0
            for c in range(3):
                d = abs(row_a[x * ca + c] - row_b[x * cb + c])
                total += d
                worst = max(worst, d)
                diff[x * 3 + c] = min(255, d * 8)
            if worst > threshold:
                over += 1
        diff_rows.append(diff)
    return total / (w * h * 3), over / (w * h), diff_rows


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('reference')
    parser.add_argument('new')
    parser.add_argument('--out', help='folder for difference images')
    parser.add_argument('--threshold', type=int, default=16,
                        help='channel difference that counts a pixel as changed (default 16)')
    parser.add_argument('--tolerance', type=float, default=1.0,
                        help='percent of changed pixels allowed per capture (default 1.0)')
    args = parser.parse_args()

    names = sorted(set(n for n in os.listdir(args.reference) if n.endswith('.png'))
                   | set(n for n in os.listdir(args.new) if n.endswith('.png')))
    if args.out:
        os.makedirs(args.out, exist_ok=True)

    failed = False
    print(f'{"capture":20s} {"mean diff":>10s} {"changed":>9s}')
    for name in names:
        a, b = os.path.join(args.reference, name), os.path.join(args.new, name)
        if not os.path.exists(a) or not os.path.exists(b):
            print(f'{name:20s} missing from {"reference" if not os.path.exists(a) else "new"}')
            failed = True
            continue
        try:
            mean, changed, diff = compare(read_png(a), read_png(b), args.threshold)
        except ValueError as e:
            print(f'{name:20s} {e}')
            failed = True
            continue
        bad = changed * 100 > args.tolerance
        failed |= bad
        print(f'{name:20s} {mean:10.2f} {changed * 100:8.2f}%{"  OVER" if bad else ""}')
        if bad and args.out:
            write_png(os.path.join(args.out, name), len(diff[0]) // 3, len(diff), diff)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
