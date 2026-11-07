#!/usr/bin/env python3
"""
Thử giao thức BLE của thiết bị "NavESP" từ máy Mac (docs/ble_protocol.md):
gửi tuyến RTE1, nhận trạng thái, gửi lệnh.

  .venv/bin/python send_route.py route.bin            # gửi tuyến rồi theo dõi trạng thái 20 s
  .venv/bin/python send_route.py --cmd 4               # chỉ gửi lệnh (4 = bật SIM), xem CMD trong ble_protocol.md
  .venv/bin/python send_route.py --watch 60            # chỉ theo dõi trạng thái
"""
import argparse
import asyncio
import struct
import zlib

from bleak import BleakClient, BleakScanner

BASE = '4e415600-ba5e-4c0d-8e2f-6a1b3c5d7e9{}'
SVC, ROUTE, STATUS, CONTROL = (BASE.format(c) for c in '0123')
STATES = {0: 'idle', 1: 'navigating', 2: 'off-route', 3: 'arrived'}
UPLOADS = {0: 'none', 1: 'receiving', 2: 'ok', 3: 'err-crc', 4: 'err-format', 5: 'err-size', 6: 'err-sequence'}


def decode_status(b):
    v, state, flags, upload, lat, lon, spd, nxt, dnext, remain, crc = struct.unpack('<BBBBiiHHIII', bytes(b)[:28])
    return (f'state={STATES.get(state, state)} upload={UPLOADS.get(upload, upload)} flags={flags:04b} '
            f'pos=({lat / 1e7:.6f},{lon / 1e7:.6f}) v={spd * 0.036:.1f}km/h next={nxt if nxt != 0xFFFF else "-"} '
            f'dnext={dnext}m remain={remain}m crc={crc:08x}')


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('route', nargs='?')
    ap.add_argument('--cmd', type=int)
    ap.add_argument('--watch', type=float, default=20)
    ap.add_argument('--chunk', type=int, default=180)
    args = ap.parse_args()

    dev = await BleakScanner.find_device_by_filter(lambda d, ad: SVC in [u.lower() for u in ad.service_uuids] or d.name == 'NavESP',
                                                   timeout=15)
    if not dev:
        raise SystemExit('Khong tim thay NavESP')
    async with BleakClient(dev) as cli:
        print(f'Da ket noi {dev.name} {dev.address}, MTU {cli.mtu_size}')
        await cli.start_notify(STATUS, lambda _, data: print('  status:', decode_status(bytes(data))))
        print('  doc status:', decode_status(bytes(await cli.read_gatt_char(STATUS))))
        if args.cmd is not None:
            await cli.write_gatt_char(CONTROL, bytes([args.cmd]), response=True)
            print(f'Da gui lenh {args.cmd}')
        if args.route:
            data = open(args.route, 'rb').read()
            crc = zlib.crc32(data)
            n = min(args.chunk, cli.mtu_size - 3 - 5)
            await cli.write_gatt_char(ROUTE, struct.pack('<BII', 1, len(data), crc), response=True)
            for off in range(0, len(data), n):
                await cli.write_gatt_char(ROUTE, struct.pack('<BI', 2, off) + data[off:off + n], response=True)
            await cli.write_gatt_char(ROUTE, bytes([3]), response=True)
            print(f'Da gui tuyen {len(data)} byte, CRC {crc:08x}, goi {n} byte')
        await asyncio.sleep(args.watch)


asyncio.run(main())
