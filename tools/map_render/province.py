#!/usr/bin/env python3
"""
Lấy ranh giới tỉnh (relation OSM) từ file .osm.pbf, lưu out/province.geojson và đếm số tile cần render.
  python province.py data/vietnam-latest.osm.pbf 1884034
"""
import json
import math
import sys
from pathlib import Path

import osmium
from shapely.geometry import MultiPolygon, Polygon, box, mapping

src, rel_id = sys.argv[1], int(sys.argv[2])
polys = []
fp = (osmium.FileProcessor(src).with_areas(osmium.filter.KeyFilter('boundary'))
      .with_filter(osmium.filter.KeyFilter('boundary')))
for o in fp:
    if o.is_area() and not o.from_way() and o.orig_id() == rel_id:
        for outer in o.outer_rings():
            ring = [(n.lon, n.lat) for n in outer]
            holes = [[(n.lon, n.lat) for n in inner] for inner in o.inner_rings(outer)]
            polys.append(Polygon(ring, holes))
geom = MultiPolygon(polys)
out = Path(__file__).parent / 'out' / 'province.geojson'
out.write_text(json.dumps({'type': 'Feature', 'properties': {'osm_relation': rel_id}, 'geometry': mapping(geom)}))
lon0, lat0, lon1, lat1 = geom.bounds
km2 = geom.area * (111.32 ** 2) * math.cos(math.radians((lat0 + lat1) / 2))
print(f'bbox lat {lat0:.3f}..{lat1:.3f} lon {lon0:.3f}..{lon1:.3f}, dien tich ~{km2:,.0f} km2')

def tile_bounds(z, x, y):
    n = 2 ** z
    lo0, lo1 = x / n * 360 - 180, (x + 1) / n * 360 - 180
    la1 = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))
    la0 = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (y + 1) / n))))
    return lo0, la0, lo1, la1

def deg2tile(lat, lon, z):
    n = 2 ** z
    return int((lon + 180) / 360 * n), int((1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n)

from shapely.prepared import prep
pg = prep(geom.buffer(0.005))
for z in (14, 15, 16, 17):
    x0, y0 = deg2tile(lat1, lon0, z)
    x1, y1 = deg2tile(lat0, lon1, z)
    n = sum(1 for x in range(x0, x1 + 1) for y in range(y0, y1 + 1) if pg.intersects(box(*tile_bounds(z, x, y))))
    print(f'z{z}: {n} tile')
