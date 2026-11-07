#!/usr/bin/env python3
"""
Tự render tile bản đồ tối giản từ dữ liệu OSM (không có chữ) + xuất danh sách nhãn tên đường.

  python render.py <file .osm.pbf/.osm> [--tiles-from ../map_tiles/png] [--zooms 14-17] [--out out]

Kết quả:
  out/png/{z}/{x}/{y}.png   - tile 256x256, kiểu tối giản cho màn 2.8" trên xe máy, KHÔNG có chữ
  out/labels.csv            - nhãn tên đường (xem README.md)

Pipeline: pyosmium đọc dữ liệu (ghép multipolygon thành vùng) -> Pillow vẽ ở 2x rồi thu nhỏ (khử răng cưa).
Chỉ cần Python + osmium + Pillow, không cần Mapnik/tile server.
"""
import argparse
import csv
import math
import sys
import time
from collections import defaultdict
from pathlib import Path

import osmium
from PIL import Image, ImageDraw

TILE = 256
SS = 2                      # vẽ ở 2x rồi thu nhỏ để khử răng cưa

# ---------- Kiểu hiển thị ----------
BG = (236, 233, 226)
WATER = (140, 190, 240)
GREEN = (205, 232, 190)
BUILDING = (218, 212, 203)

# loại đường -> (lớp vẽ, màu lõi, màu viền, {zoom: độ rộng lõi px}); không có zoom = không vẽ.
# Lõi mảnh hơn MIN_CASED px thì vẽ thành nét liền màu viền (không viền) để vẫn rõ ở zoom nhỏ.
ROADS = {
    'motorway':     (6, (240, 140, 40), (160, 80, 10),    {14: 4, 15: 6, 16: 9, 17: 13}),
    'trunk':        (6, (240, 140, 40), (160, 80, 10),    {14: 4, 15: 6, 16: 9, 17: 13}),
    'primary':      (5, (248, 178, 90), (170, 100, 30),   {14: 3.5, 15: 5, 16: 8, 17: 12}),
    'secondary':    (4, (250, 220, 110), (160, 130, 40),  {14: 3, 15: 4.5, 16: 7, 17: 10}),
    'tertiary':     (3, (255, 255, 255), (130, 130, 130), {14: 2, 15: 3.5, 16: 6, 17: 9}),
    'residential':  (2, (255, 255, 255), (165, 165, 165), {14: 1, 15: 2, 16: 4.5, 17: 7}),
    'unclassified': (2, (255, 255, 255), (165, 165, 165), {14: 1, 15: 2, 16: 4.5, 17: 7}),
    'living_street': (2, (255, 255, 255), (165, 165, 165), {15: 2, 16: 4.5, 17: 7}),
    'service':      (1, (255, 255, 255), (185, 185, 185), {16: 2, 17: 3.5}),
    'track':        (1, (175, 155, 125), None,            {16: 1.2, 17: 1.8}),
    'path':         (1, (175, 155, 125), None,            {17: 1.2}),
    'footway':      (1, (175, 155, 125), None,            {17: 1.2}),
}
# Kiểu đêm: cùng lớp và độ rộng, chỉ đổi màu. Đường xám sáng dần theo cấp, xanh của đường dẫn vẫn nổi.
NIGHT_BASE = {'BG': (20, 22, 28), 'WATER': (20, 44, 78), 'GREEN': (24, 44, 34), 'BUILDING': (36, 38, 46)}
NIGHT_ROADS = {
    'motorway': ((225, 225, 230), (120, 122, 130)), 'trunk': ((225, 225, 230), (120, 122, 130)),
    'primary': ((205, 205, 212), (105, 107, 116)),
    'secondary': ((180, 182, 190), (92, 94, 104)),
    'tertiary': ((150, 152, 162), (82, 84, 94)),
    'residential': ((118, 120, 130), (72, 74, 84)), 'unclassified': ((118, 120, 130), (72, 74, 84)),
    'living_street': ((118, 120, 130), (72, 74, 84)),
    'service': ((92, 94, 104), (60, 62, 72)),
    'track': ((78, 74, 68), None), 'path': ((78, 74, 68), None), 'footway': ((78, 74, 68), None),
}


def use_night_style():
    globals().update(NIGHT_BASE)
    for k, (core, casing) in NIGHT_ROADS.items():
        layer, _, _, widths = ROADS[k]
        ROADS[k] = (layer, core, casing, widths)
        if k + '_link' in ROADS:
            ROADS[k + '_link'] = ROADS[k]


MIN_CASED = 2.5
CASING_PX = 2
for _k in ('motorway', 'trunk', 'primary', 'secondary', 'tertiary'):
    ROADS[_k + '_link'] = ROADS[_k]
WATERWAYS = {'river': {14: 2, 15: 3, 16: 4, 17: 6}, 'canal': {15: 2, 16: 3, 17: 4},
             'stream': {16: 1, 17: 1.5}}
GREEN_TAGS = {('leisure', 'park'), ('leisure', 'garden'), ('landuse', 'forest'), ('natural', 'wood'),
              ('landuse', 'grass'), ('leisure', 'pitch'), ('landuse', 'recreation_ground')}
# ưu tiên nhãn: số nhỏ = quan trọng hơn
LABEL_PRIO = {'motorway': 1, 'trunk': 1, 'primary': 2, 'secondary': 3, 'tertiary': 4,
              'residential': 5, 'unclassified': 5, 'living_street': 6}


# ---------- Toạ độ ----------
def to_world(lat, lon, z):
    """lat/lon -> toạ độ pixel thế giới ở zoom z (tile 256)."""
    n = TILE * 2 ** z
    x = (lon + 180.0) / 360.0 * n
    y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n
    return x, y


def bearing(lat1, lon1, lat2, lon2):
    """Hướng đoạn thẳng (độ, 0 = bắc, theo chiều kim đồng hồ)."""
    dy = lat2 - lat1
    dx = (lon2 - lon1) * math.cos(math.radians((lat1 + lat2) / 2))
    return math.degrees(math.atan2(dx, dy)) % 360


def dist_m(lat1, lon1, lat2, lon2):
    dy = (lat2 - lat1) * 111320
    dx = (lon2 - lon1) * 111320 * math.cos(math.radians(lat1))
    return math.hypot(dx, dy)


# ---------- Đọc dữ liệu OSM ----------
def load(path, bbox):
    """Trả về dict các lớp: roads, waterways (đường) và water, green, buildings (vùng). Toạ độ (lat, lon)."""
    lat0, lat1, lon0, lon1 = bbox
    data = defaultdict(list)

    def inside(coords):
        for la, lo in coords:
            if lat0 <= la <= lat1 and lon0 <= lo <= lon1:
                return True
        return False

    fp = (osmium.FileProcessor(path)
          .with_locations()
          .with_areas(osmium.filter.KeyFilter('natural', 'landuse', 'leisure', 'building', 'water'))
          .with_filter(osmium.filter.KeyFilter('highway', 'waterway', 'natural', 'landuse', 'leisure',
                                               'building', 'water')))
    for obj in fp:
        tags = obj.tags
        if obj.is_way():
            hw = tags.get('highway')
            ww = tags.get('waterway')
            if not hw and not ww:
                continue
            try:
                coords = [(n.lat, n.lon) for n in obj.nodes]
            except osmium.InvalidLocationError:
                continue
            if len(coords) < 2 or not inside(coords):
                continue
            if hw in ROADS:
                data['roads'].append((hw, tags.get('name', ''), coords))
            elif ww in WATERWAYS:
                data['waterways'].append((ww, coords))
        elif obj.is_area():
            kind = None
            if tags.get('natural') == 'water' or tags.get('landuse') in ('reservoir', 'basin') or \
                    tags.get('waterway') == 'riverbank':
                kind = 'water'
            elif any((k, tags.get(k)) in GREEN_TAGS for k in ('leisure', 'landuse', 'natural')):
                kind = 'green'
            elif tags.get('building') and tags.get('building') != 'no':
                kind = 'buildings'
            if not kind:
                continue
            polys = []
            for outer in obj.outer_rings():
                ring = [(n.lat, n.lon) for n in outer]
                holes = [[(n.lat, n.lon) for n in inner] for inner in obj.inner_rings(outer)]
                if inside(ring):
                    polys.append((ring, holes))
            if polys:
                data[kind].append(polys)
    return data


# ---------- Vẽ tile ----------
def index_features(items, coords_of):
    """Chỉ mục thô theo ô z14 để chỉ duyệt các đối tượng gần tile."""
    idx = defaultdict(list)
    for it in items:
        cs = coords_of(it)
        xs, ys = zip(*(to_world(la, lo, 14) for la, lo in cs))
        for tx in range(int(min(xs) // TILE), int(max(xs) // TILE) + 1):
            for ty in range(int(min(ys) // TILE), int(max(ys) // TILE) + 1):
                idx[(tx, ty)].append(it)
    return idx


def render_tile(z, x, y, idx):
    size = TILE * SS
    img = Image.new('RGB', (size, size), BG)
    d = ImageDraw.Draw(img)
    ox, oy = x * TILE, y * TILE
    k14 = (x >> (z - 14), y >> (z - 14))

    def px(cs):
        out = []
        for la, lo in cs:
            wx, wy = to_world(la, lo, z)
            out.append(((wx - ox) * SS, (wy - oy) * SS))
        return out

    # Vùng: cây xanh, nước, nhà (nhà chỉ ở z17)
    for kind, color in (('green', GREEN), ('water', WATER), ('buildings', BUILDING)):
        if kind == 'buildings' and z < 17:
            continue
        for polys in idx[kind].get(k14, ()):
            for ring, holes in polys:
                d.polygon(px(ring), fill=color)
                for h in holes:
                    d.polygon(px(h), fill=BG)

    # Sông suối
    for ww, cs in idx['waterways'].get(k14, ()):
        w = WATERWAYS[ww].get(z)
        if w:
            d.line(px(cs), fill=WATER, width=max(1, round(w * SS)), joint='curve')

    # Đường: vẽ viền rồi lõi, theo lớp từ nhỏ tới lớn để đường lớn nằm trên
    roads = [r for r in idx['roads'].get(k14, ()) if z in ROADS[r[0]][3]]
    roads.sort(key=lambda r: ROADS[r[0]][0])
    for layer in sorted({ROADS[r[0]][0] for r in roads}):
        group = [r for r in roads if ROADS[r[0]][0] == layer]
        for hw, _, cs in group:
            _, core, casing, widths = ROADS[hw]
            if casing and widths[z] >= MIN_CASED:
                w = round((widths[z] + CASING_PX) * SS)
                pts = px(cs)
                d.line(pts, fill=casing, width=w, joint='curve')
                r = w / 2
                for p in (pts[0], pts[-1]):      # bo tròn đầu đường
                    d.ellipse((p[0] - r, p[1] - r, p[0] + r, p[1] + r), fill=casing)
        for hw, _, cs in group:
            _, core, casing, widths = ROADS[hw]
            w = max(1, round(widths[z] * SS))
            if casing and widths[z] < MIN_CASED:
                core = casing                      # đường mảnh: nét liền màu đậm, không viền
            pts = px(cs)
            d.line(pts, fill=core, width=w, joint='curve')
            r = w / 2
            for p in (pts[0], pts[-1]):
                d.ellipse((p[0] - r, p[1] - r, p[0] + r, p[1] + r), fill=core)

    return img.resize((TILE, TILE), Image.LANCZOS)


# ---------- Nhãn tên đường ----------
def export_labels(roads, path, spacing_m=300, dedupe_m=150):
    """Mỗi đường có tên: đặt điểm nhãn mỗi ~spacing_m dọc theo đường (giữa đoạn thẳng đủ dài), bỏ trùng tên gần nhau."""
    labels = []
    for hw, name, cs in roads:
        if not name or hw not in LABEL_PRIO:
            continue
        acc = spacing_m / 2
        for (la1, lo1), (la2, lo2) in zip(cs, cs[1:]):
            seg = dist_m(la1, lo1, la2, lo2)
            if seg < 1:
                continue
            acc += seg
            if acc >= spacing_m and seg >= 25:
                labels.append((name, (la1 + la2) / 2, (lo1 + lo2) / 2, bearing(la1, lo1, la2, lo2) % 180,
                               LABEL_PRIO[hw], hw))
                acc = 0
    labels.sort(key=lambda l: l[4])
    kept = []
    grid = defaultdict(list)                     # ô ~0.002 độ để bỏ trùng nhanh
    for l in labels:
        cell = (round(l[1] / 0.002), round(l[2] / 0.002))
        near = [k for dx in (-1, 0, 1) for dy in (-1, 0, 1) for k in grid[(cell[0] + dx, cell[1] + dy)]]
        if any(k[0] == l[0] and dist_m(k[1], k[2], l[1], l[2]) < dedupe_m for k in near):
            continue
        grid[cell].append(l)
        kept.append(l)
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(['name', 'lat', 'lon', 'bearing_deg', 'priority', 'highway'])
        for name, la, lo, b, p, hw in kept:
            w.writerow([name, f'{la:.6f}', f'{lo:.6f}', f'{b:.0f}', p, hw])
    return kept


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('osm')
    ap.add_argument('--tiles-from', default=str(Path(__file__).parent.parent / 'map_tiles' / 'png'),
                    help='thư mục z/x/y.png để lấy danh sách tile cần render (giống bộ OSM hiện có)')
    ap.add_argument('--zooms', default='14-17')
    ap.add_argument('--out', default=str(Path(__file__).parent / 'out'))
    ap.add_argument('--style', choices=['day', 'night'], default='day')
    ap.add_argument('--near', help='lat,lon,bán kính tile: chỉ render quanh điểm này, để làm mẫu')
    args = ap.parse_args()
    if args.style == 'night':
        use_night_style()
    z0, z1 = map(int, args.zooms.split('-'))
    out = Path(args.out)

    tiles = sorted((int(p.parts[-3]), int(p.parts[-2]), int(p.stem))
                   for p in Path(args.tiles_from).glob('*/*/*.png'))
    tiles = [t for t in tiles if z0 <= t[0] <= z1]
    if args.near:
        la, lo, rad = args.near.split(',')
        la, lo, rad = float(la), float(lo), int(rad)

        def center(z):
            n = 2 ** z
            return int((lo + 180) / 360 * n), int((1 - math.asinh(math.tan(math.radians(la))) / math.pi) / 2 * n)
        tiles = [t for t in tiles if abs(t[1] - center(t[0])[0]) <= rad and abs(t[2] - center(t[0])[1]) <= rad]

    def tile_bbox(z, x, y):
        n = 2 ** z
        lon0, lon1 = x / n * 360 - 180, (x + 1) / n * 360 - 180
        lat1 = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))
        lat0 = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (y + 1) / n))))
        return lat0, lat1, lon0, lon1

    bbs = [tile_bbox(*t) for t in tiles]
    m = 0.005                                     # lề ~500 m để đối tượng cắt qua mép tile vẫn được vẽ
    bbox = (min(b[0] for b in bbs) - m, max(b[1] for b in bbs) + m,
            min(b[2] for b in bbs) - m, max(b[3] for b in bbs) + m)
    print(f'{len(tiles)} tile z{z0}-{z1}, bbox lat {bbox[0]:.4f}..{bbox[1]:.4f} lon {bbox[2]:.4f}..{bbox[3]:.4f}')

    t0 = time.time()
    data = load(args.osm, bbox)
    print(f'Doc OSM: {time.time() - t0:.0f} s; ' + ', '.join(f'{k} {len(v)}' for k, v in data.items()))

    t0 = time.time()
    idx = {
        'roads': index_features(data['roads'], lambda r: r[2]),
        'waterways': index_features(data['waterways'], lambda r: r[1]),
        'water': index_features(data['water'], lambda p: [c for ring, _ in p for c in ring]),
        'green': index_features(data['green'], lambda p: [c for ring, _ in p for c in ring]),
        'buildings': index_features(data['buildings'], lambda p: [c for ring, _ in p for c in ring]),
    }
    for i, (z, x, y) in enumerate(tiles):
        p = out / 'png' / str(z) / str(x) / f'{y}.png'
        p.parent.mkdir(parents=True, exist_ok=True)
        render_tile(z, x, y, idx).save(p, optimize=True)
    print(f'Render {len(tiles)} tile: {time.time() - t0:.0f} s')

    labels = export_labels(data['roads'], out / 'labels.csv')
    print(f'Nhan: {len(labels)} -> {out / "labels.csv"}')


if __name__ == '__main__':
    sys.exit(main())
