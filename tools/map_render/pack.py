#!/usr/bin/env python3
"""
Gộp tile rời thành gói theo ô z14 cho thẻ SD (firmware/nav đọc mọi thư mục /nav_map_* và /navmap cũ, xem pack_get trong main.c).

  python pack.py --day out/tiles_min_lz4 [--night <thư mục tile đêm>] [--osm data/area.osm] --out <thư mục>/nav_map_<tên>

Mỗi ô z14 có tile ở bất kỳ mức z14-17 thành một file <out>/<day|night>/<x14>/<y14>.pak:
  "NPK1", u16 n, u16 0, u32 str_off, u32 str_len
  n x {u8 z, u8 rx, u8 ry, u8 0, u32 off, u32 len}      rx, ry: vị trí tile trong ô (0 .. 2^(z-14)-1)
  dữ liệu tile: y hệt file .bin rời (ảnh LVGL RGB565 nén LZ4)
  phần tên đường của ô (chỉ gói ngày): khối "STR1" như streets_bin.py nhưng chỉ gồm nhãn nằm trong ô
  và các đường có khung bao chạm ô (tên trong khối là tên cục bộ của ô).
Thẻ FAT32 chép mỗi file chậm (~25 file/giây), gộp lại thì số file giảm khoảng 85 lần ở z17.
"""
import argparse
import math
import struct
import tempfile
from collections import defaultdict
from pathlib import Path

import render
from streets_bin import simplify

TILE = 256


def tiles_in(root):
    """{(x14, y14): [(z, x, y, path)]} cho các tile z14-17 trong thư mục z/x/y.bin"""
    cells = defaultdict(list)
    if not root:
        return cells
    for p in Path(root).glob('*/*/*.bin'):
        z, x, y = int(p.parts[-3]), int(p.parts[-2]), int(p.stem)
        if 14 <= z <= 17:
            sh = z - 14
            cells[(x >> sh, y >> sh)].append((z, x, y, p))
    return cells


def cell_of(lat, lon):
    n = 2 ** 14
    x = (lon + 180) / 360 * n
    y = (1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n
    return int(x), int(y)


def streets_chunks(osm, cells):
    """{(x14, y14): bytes STR1} cho các ô trong `cells`, đọc tên đường từ file OSM"""
    return streets_chunks_from(render.load(osm, (-90, 90, -180, 180)), cells)


def streets_chunks_from(data, cells):
    """Như streets_chunks nhưng dùng dữ liệu render.load() đã nạp sẵn"""
    named = [r for r in data['roads'] if r[1] and r[0] in render.LABEL_PRIO]
    with tempfile.NamedTemporaryFile(suffix='.csv') as tmp:
        labels = render.export_labels(data['roads'], tmp.name)
    per_labels = defaultdict(list)
    for lab in labels:
        c = cell_of(lab[1], lab[2])
        if c in cells:
            per_labels[c].append(lab)
    per_roads = defaultdict(list)
    for hw, name, cs in named:
        cs = simplify(cs)
        xs, ys = zip(*(cell_of(la, lo) for la, lo in cs))
        for cx in range(min(xs), max(xs) + 1):
            for cy in range(min(ys), max(ys) + 1):
                if (cx, cy) in cells:
                    per_roads[(cx, cy)].append((name, render.LABEL_PRIO[hw], cs))
    out = {}
    for c in cells:
        labs, roads = per_labels.get(c, []), per_roads.get(c, [])
        names = sorted({l[0] for l in labs} | {r[0] for r in roads})
        nid = {n: i for i, n in enumerate(names)}
        b = bytearray(b'STR1') + struct.pack('<H', len(names))
        for n in names:
            e = n.encode('utf-8')[:255]
            b += struct.pack('<B', len(e)) + e
        b += struct.pack('<I', len(labs))
        for name, la, lo, brg, prio, _ in labs:
            b += struct.pack('<iiHBB', round(la * 1e7), round(lo * 1e7), nid[name], int(brg) % 180, prio)
        b += struct.pack('<I', len(roads))
        for name, prio, cs in roads:
            b += struct.pack('<HBBH', nid[name], prio, 0, len(cs))
            for la, lo in cs:
                b += struct.pack('<ii', round(la * 1e7), round(lo * 1e7))
        out[c] = bytes(b)
    return out


def write_pack(path, tiles, streets=b''):
    tiles = sorted(tiles)
    head = 16 + 12 * len(tiles)
    blobs = [p.read_bytes() for *_, p in tiles]
    off = head
    idx = bytearray()
    for (z, x, y, _), blob in zip(tiles, blobs):
        sh = z - 14
        idx += struct.pack('<BBBBII', z, x - ((x >> sh) << sh), y - ((y >> sh) << sh), 0, off, len(blob))
        off += len(blob)
    str_off = off if streets else 0
    data = b'NPK1' + struct.pack('<HHII', len(tiles), 0, str_off, len(streets)) + idx + b''.join(blobs) + streets
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--day', required=True, help='thư mục tile ngày z/x/y.bin (tiles_min_lz4)')
    ap.add_argument('--night', help='thư mục tile đêm z/x/y.bin')
    ap.add_argument('--osm', help='file .osm/.osm.pbf để lấy tên đường (không có thì gói không có tên đường)')
    ap.add_argument('--out', required=True, help='thư mục đích nav_map_<tên> (chép nguyên thư mục này vào gốc thẻ SD)')
    a = ap.parse_args()
    out = Path(a.out)
    day, night = tiles_in(a.day), tiles_in(a.night)
    streets = streets_chunks(a.osm, set(day)) if a.osm else {}
    total = n_tiles = 0
    for c, tiles in day.items():
        total += write_pack(out / 'day' / str(c[0]) / f'{c[1]}.pak', tiles, streets.get(c, b''))
        n_tiles += len(tiles)
    for c, tiles in night.items():
        total += write_pack(out / 'night' / str(c[0]) / f'{c[1]}.pak', tiles)
        n_tiles += len(tiles)
    print(f'{out}: {len(day)} goi ngay, {len(night)} goi dem, {n_tiles} tile, '
          f'{sum(len(v) for v in streets.values()) / 1024:.0f} KB ten duong, tong {total / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
