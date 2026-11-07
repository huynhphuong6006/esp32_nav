# Thử giao thức Bluetooth từ máy tính

`send_route.py` (Python 3 + [bleak](https://github.com/hbldh/bleak)): kết nối thiết bị "NavESP", gửi một file tuyến RTE1 (tạo bằng `tools/map_render/route_bin.py`), đọc trạng thái. Giao thức: `docs/ble_protocol.md`.

```
pip install bleak
python3 send_route.py route.bin
```

macOS: chạy từ Terminal có quyền Bluetooth.
