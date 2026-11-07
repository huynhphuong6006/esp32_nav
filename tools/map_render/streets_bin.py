#!/usr/bin/env python3
"""
Xuất dữ liệu tên đường cho ESP32 thành 1 file nhị phân streets.bin (little-endian):

  "STR1"
  u16 n_names, rồi n_names x (u8 len, len byte UTF-8)          - bảng tên đường
  u32 n_labels, rồi n_labels x (i32 lat_e7, i32 lon_e7, u16 name_id, u8 bearing 0..179, u8 priority)
                                                              - điểm đặt nhãn, đã sắp theo priority tăng dần
  u32 n_roads, rồi n_roads x (u16 name_id, u8 priority, u8 0, u16 n_pts, n_pts x (i32 lat_e7, i32 lon_e7))
                                                              - đường có tên (đã giản lược ~3 m), để tìm tên đường đang đi

  python streets_bin.py <file .osm/.osm.pbf> [out/streets.bin]
"""
import math
import struct
import sys
from pathlib import Path

import render


def simplify(cs, tol_m=3.0):
    """Douglas-Peucker trên toạ độ phẳng cục bộ (mét)."""
    if len(cs) <= 2:
        return cs
    lat0 = math.radians(cs[0][0])
    pts = [(lo * 111320 * math.cos(lat0), la * 111320) for la, lo in cs]
    keep = [False] * len(cs)
    keep[0] = keep[-1] = True
    stack = [(0, len(cs) - 1)]
    while stack:
        a, b = stack.pop()
        (ax, ay), (bx, by) = pts[a], pts[b]
        dx, dy = bx - ax, by - ay
        L = math.hypot(dx, dy) or 1e-9
        best, bi = 0, -1
        for i in range(a + 1, b):
            d = abs((pts[i][0] - ax) * dy - (pts[i][1] - ay) * dx) / L
            if d > best:
                best, bi = d, i
        if best > tol_m:
            keep[bi] = True
            stack += [(a, bi), (bi, b)]
    return [c for c, k in zip(cs, keep) if k]


def main():
    src = sys.argv[1]
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).parent / 'out' / 'streets.bin'
    bbox = (-90, 90, -180, 180)
    data = render.load(src, bbox)
    named = [r for r in data['roads'] if r[1] and r[0] in render.LABEL_PRIO]
    labels = render.export_labels(data['roads'], dst.with_suffix('.csv'))

    names = sorted({r[1] for r in named})
    nid = {n: i for i, n in enumerate(names)}
    out = bytearray(b'STR1')
    out += struct.pack('<H', len(names))
    for n in names:
        b = n.encode('utf-8')[:255]
        out += struct.pack('<B', len(b)) + b
    out += struct.pack('<I', len(labels))
    for name, la, lo, brg, prio, _ in labels:
        out += struct.pack('<iiHBB', round(la * 1e7), round(lo * 1e7), nid[name], int(brg) % 180, prio)
    total_pts = 0
    roads = [(nid[n], render.LABEL_PRIO[hw], simplify(cs)) for hw, n, cs in named]
    out += struct.pack('<I', len(roads))
    for name_id, prio, cs in roads:
        out += struct.pack('<HBBH', name_id, prio, 0, len(cs))
        for la, lo in cs:
            out += struct.pack('<ii', round(la * 1e7), round(lo * 1e7))
        total_pts += len(cs)
    dst.write_bytes(out)
    print(f'{dst}: {len(out) / 1024:.0f} KB, {len(names)} ten, {len(labels)} nhan, {len(roads)} duong, {total_pts} diem')


if __name__ == '__main__':
    main()
