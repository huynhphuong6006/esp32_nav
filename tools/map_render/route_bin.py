#!/usr/bin/env python3
"""
Chuyển tuyến đường OSRM (JSON, geometries=geojson, steps=true) thành route.bin cho firmware/nav (little-endian):

  "RTE1"
  u32 n_pts, n_pts x (i32 lat_e7, i32 lon_e7)                 - hình dạng tuyến
  u16 n_steps, n_steps x (u32 pt_index, u8 kind, u8 exit, u8 len, len byte UTF-8 tên đường)
        pt_index: điểm của tuyến gần chỗ rẽ nhất; kind xem KIND bên dưới; exit: lối ra ở vòng xuyến (0 nếu không có)
  Khoảng cách tới chỗ rẽ thiết bị tự tính từ hình dạng tuyến. Cùng định dạng này sẽ được gửi từ điện thoại qua BLE.

  python route_bin.py [out/sim_route.json] [out/route.bin]
"""
import json
import math
import struct
import sys
from pathlib import Path

HERE = Path(__file__).parent
# Loại chỉ dẫn (trùng với enum trong firmware/nav)
KIND = {'straight': 0, 'slight right': 1, 'right': 2, 'sharp right': 3, 'uturn': 4,
        'sharp left': 5, 'left': 6, 'slight left': 7}
K_ROUNDABOUT, K_ARRIVE, K_DEPART = 8, 9, 10


def dist(a, b):
    dy = (b[0] - a[0]) * 111320
    dx = (b[1] - a[1]) * 111320 * math.cos(math.radians(a[0]))
    return math.hypot(dx, dy)


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / 'out' / 'sim_route.json'
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / 'out' / 'route.bin'
    route = json.loads(src.read_text())['routes'][0]
    pts = [(lat, lon) for lon, lat in route['geometry']['coordinates']]
    legs = route['legs']
    steps = []
    for li, leg in enumerate(legs):
        for s in leg['steps']:
            m = s['maneuver']
            t, mod = m['type'], m.get('modifier', 'straight')
            if t == 'depart':
                if li > 0:
                    continue                  # điểm trung gian: bỏ "đến"/"xuất phát" giữa các chặng
                kind = K_DEPART
            elif t == 'arrive':
                if li < len(legs) - 1:
                    continue
                kind = K_ARRIVE
            elif t in ('roundabout', 'rotary', 'roundabout turn'):
                kind = K_ROUNDABOUT
            elif t in ('new name', 'continue') and mod in ('straight', 'slight left', 'slight right'):
                continue                      # đi thẳng/đổi tên đường: không cần báo
            else:
                kind = KIND.get(mod, 0)
            lat, lon = m['location'][1], m['location'][0]
            idx = min(range(len(pts)), key=lambda i: dist(pts[i], (lat, lon)))
            steps.append((idx, kind, m.get('exit', 0) if kind == K_ROUNDABOUT else 0, s.get('name') or ''))

    out = bytearray(b'RTE1') + struct.pack('<I', len(pts))
    for la, lo in pts:
        out += struct.pack('<ii', round(la * 1e7), round(lo * 1e7))
    out += struct.pack('<H', len(steps))
    for idx, kind, ex, name in steps:
        b = name.encode('utf-8')[:255]
        out += struct.pack('<IBBB', idx, kind, ex, len(b)) + b
    dst.write_bytes(out)
    names = {v: k for k, v in KIND.items()} | {K_ROUNDABOUT: 'roundabout', K_ARRIVE: 'arrive', K_DEPART: 'depart'}
    print(f'{dst}: {len(pts)} diem, {len(steps)} buoc, {route["distance"]:.0f} m, {len(out)} byte')
    for idx, kind, ex, name in steps:
        print(f'  #{idx:4d} {names[kind]:12s} {name}')


if __name__ == '__main__':
    main()
