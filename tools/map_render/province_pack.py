#!/usr/bin/env python3
"""
Render một tỉnh thành gói bản đồ cho thẻ SD (thư mục nav_map_<tỉnh>, xem pack.py), kèm manifest.json cho trang tải.

  python province_pack.py data/vietnam-latest.osm.pbf --geojson out/province.geojson --name dak-lak \\
         [--zooms 14-16] [--z17-city-km 4 --z17-town-km 2] [--no-night] [--workers 7]

- z14..z16: mọi tile chạm ranh giới tỉnh (nới ~500 m).
- z17: chỉ quanh các điểm place=city (bán kính --z17-city-km) và place=town (--z17-town-km) trong tỉnh.
- Tile không có gì (toàn màu nền) thì bỏ, thiết bị tô màu nền thay.
- Kết quả: out/provinces/<name>/nav_map_<name bỏ dấu ->/{day,night}/<x14>/<y14>.pak và out/provinces/<name>/manifest.json.
  Chép nguyên thư mục nav_map_<...> vào gốc thẻ SD; nhiều tỉnh thì nhiều thư mục, firmware đọc hết.
"""
import argparse
import hashlib
import json
import math
import multiprocessing as mp
import struct
import time
from collections import defaultdict
from pathlib import Path

import lz4.block
import numpy as np
import osmium
from shapely.geometry import Point, box, shape
from shapely.prepared import prep

import pack
import render

HERE = Path(__file__).parent
TILE = 256


def tile_bounds(z, x, y):
    n = 2 ** z
    lo0, lo1 = x / n * 360 - 180, (x + 1) / n * 360 - 180
    la1 = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))
    la0 = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (y + 1) / n))))
    return lo0, la0, lo1, la1


def deg2tile(lat, lon, z):
    n = 2 ** z
    return int((lon + 180) / 360 * n), int((1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n)


def tiles_for(geom, zooms, places, city_km, town_km):
    lon0, lat0, lon1, lat1 = geom.bounds
    pg = prep(geom.buffer(0.005))
    out = []
    for z in zooms:
        x0, y0 = deg2tile(lat1, lon0, z)
        x1, y1 = deg2tile(lat0, lon1, z)
        out += [(z, x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)
                if pg.intersects(box(*tile_bounds(z, x, y)))]
    if places:
        circles = [Point(lo, la).buffer((city_km if kind == 'city' else town_km) / 111.0) for kind, la, lo in places]
        x0, y0 = deg2tile(lat1, lon0, 17)
        x1, y1 = deg2tile(lat0, lon1, 17)
        z17 = set()
        for c in circles:
            cl0, ca0, cl1, ca1 = c.bounds
            tx0, ty0 = deg2tile(ca1, cl0, 17)
            tx1, ty1 = deg2tile(ca0, cl1, 17)
            pc = prep(c)
            z17 |= {(17, x, y) for x in range(tx0, tx1 + 1) for y in range(ty0, ty1 + 1)
                    if pc.intersects(box(*tile_bounds(17, x, y)))}
        out += sorted(z17)
    return out


def load_places(path, geom):
    pg = prep(geom)
    places = []
    fp = osmium.FileProcessor(path, osmium.osm.NODE).with_filter(osmium.filter.KeyFilter('place'))
    for n in fp:
        kind = n.tags.get('place')
        if kind in ('city', 'town') and pg.contains(Point(n.location.lon, n.location.lat)):
            places.append((kind, n.location.lat, n.location.lon))
    return places


def to_lvgl_bin(img):
    """Ảnh PIL RGB 256x256 -> file .bin LVGL 9 (RGB565 little-endian, nén LZ4), giống LVGLImage.py"""
    a = np.asarray(img, dtype=np.uint16)
    px = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
    raw = px.astype('<u2').tobytes()
    comp = lz4.block.compress(raw, store_size=False)
    head = struct.pack('<BBHHHHH', 0x19, 0x12, 0x0008, TILE, TILE, TILE * 2, 0)
    return head + struct.pack('<III', 2, len(comp), len(raw)) + comp


_IDX = None


def _render(job):
    z, x, y, dst = job
    img = render.render_tile(z, x, y, _IDX)
    if all(e[0] == e[1] == b for e, b in zip(img.getextrema(), render.BG)):
        return 0                                   # toàn nền: bỏ
    p = Path(dst) / str(z) / str(x) / f'{y}.bin'
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(to_lvgl_bin(img))
    return 1


def render_set(tiles, dst, workers):
    ctx = mp.get_context('fork')                   # tiến trình con dùng chung chỉ mục dữ liệu đã nạp
    with ctx.Pool(workers) as pool:
        kept = 0
        for i, k in enumerate(pool.imap_unordered(_render, [(z, x, y, str(dst)) for z, x, y in tiles], chunksize=32)):
            kept += k
            if (i + 1) % 5000 == 0:
                print(f'  {i + 1}/{len(tiles)} tile', flush=True)
    return kept


def map_folder(name):
    return 'nav_map_' + name.replace('-', '')


def manifest(root, name, extra):
    files = []
    for p in sorted((root / map_folder(name)).rglob('*.pak')):
        b = p.read_bytes()
        files.append({'path': str(p.relative_to(root)), 'size': len(b), 'md5': hashlib.md5(b).hexdigest()})
    m = {'name': name, 'folder': map_folder(name), 'built': time.strftime('%Y-%m-%d'), 'files': files,
         'total_bytes': sum(f['size'] for f in files), **extra}
    (root / 'manifest.json').write_text(json.dumps(m, ensure_ascii=False, indent=1))
    return m


def main():
    global _IDX
    ap = argparse.ArgumentParser()
    ap.add_argument('osm')
    ap.add_argument('--geojson', required=True, help='ranh giới tỉnh (province.py tạo ra)')
    ap.add_argument('--name', required=True)
    ap.add_argument('--zooms', default='14-16', help='các mức phủ cả tỉnh')
    ap.add_argument('--z17-city-km', type=float, default=4)
    ap.add_argument('--z17-town-km', type=float, default=2)
    ap.add_argument('--no-z17', action='store_true')
    ap.add_argument('--no-night', action='store_true')
    ap.add_argument('--workers', type=int, default=max(1, mp.cpu_count() - 1))
    a = ap.parse_args()
    root = HERE / 'out' / 'provinces' / a.name
    geom = shape(json.loads(Path(a.geojson).read_text())['geometry'])
    lon0, lat0, lon1, lat1 = geom.bounds

    t0 = time.time()
    places = [] if a.no_z17 else load_places(a.osm, geom)
    z0, z1 = map(int, a.zooms.split('-'))
    tiles = tiles_for(geom, range(z0, z1 + 1), places, a.z17_city_km, a.z17_town_km)
    print(f'{len(places)} diem city/town, {len(tiles)} tile can render', flush=True)

    m = 0.01
    data = render.load(a.osm, (lat0 - m, lat1 + m, lon0 - m, lon1 + m))
    print(f'Doc OSM: {time.time() - t0:.0f} s; ' + ', '.join(f'{k} {len(v)}' for k, v in data.items()), flush=True)
    _IDX = {
        'roads': render.index_features(data['roads'], lambda r: r[2]),
        'waterways': render.index_features(data['waterways'], lambda r: r[1]),
        'water': render.index_features(data['water'], lambda p: [c for ring, _ in p for c in ring]),
        'green': render.index_features(data['green'], lambda p: [c for ring, _ in p for c in ring]),
        'buildings': render.index_features(data['buildings'], lambda p: [c for ring, _ in p for c in ring]),
    }

    t1 = time.time()
    kept_day = render_set(tiles, root / 'tiles_day', a.workers)
    print(f'Ngay: {kept_day}/{len(tiles)} tile co noi dung, {time.time() - t1:.0f} s', flush=True)
    kept_night = 0
    if not a.no_night:
        t1 = time.time()
        render.use_night_style()
        kept_night = render_set(tiles, root / 'tiles_night', a.workers)
        print(f'Dem: {kept_night} tile, {time.time() - t1:.0f} s', flush=True)

    t1 = time.time()
    day, night = pack.tiles_in(root / 'tiles_day'), pack.tiles_in(root / 'tiles_night')
    streets = pack.streets_chunks_from(data, set(day))
    for c, ts in day.items():
        pack.write_pack(root / map_folder(a.name) / 'day' / str(c[0]) / f'{c[1]}.pak', ts, streets.get(c, b''))
    for c, ts in night.items():
        pack.write_pack(root / map_folder(a.name) / 'night' / str(c[0]) / f'{c[1]}.pak', ts)
    mf = manifest(root, a.name, {'zooms': a.zooms, 'z17_places': len(places), 'night': not a.no_night,
                                 'bbox': [lat0, lon0, lat1, lon1]})
    print(f'Dong goi: {time.time() - t1:.0f} s; {len(day)} goi ngay, {len(night)} goi dem, '
          f'{mf["total_bytes"] / 1e6:.0f} MB; tong {time.time() - t0:.0f} s', flush=True)


if __name__ == '__main__':
    main()
