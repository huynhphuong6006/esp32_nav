# Tạo bản đồ cho thẻ SD

Render bản đồ tối giản (không chữ) từ dữ liệu OpenStreetMap, ngày và đêm, kèm tên đường để thiết bị tự vẽ chữ đứng thẳng; đóng gói theo ô để chép vào thẻ SD nhanh.

## Cài đặt

Python 3.10+:

```
cd tools/map_render
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

Tải dữ liệu Việt Nam (khoảng 330 MB) vào `data/`:

```
mkdir -p data
curl -L -o data/vietnam-latest.osm.pbf https://download.geofabrik.de/asia/vietnam-latest.osm.pbf
```

## Tạo gói một tỉnh

1. Tìm **relation id** ranh giới tỉnh trên openstreetmap.org (tìm tên tỉnh, chọn kết quả "Ranh giới hành chính", số ở cuối địa chỉ `.../relation/<id>`). Ví dụ Đắk Lắk (địa giới mới): `1884034`.
2. Lấy ranh giới:
   ```
   .venv/bin/python province.py data/vietnam-latest.osm.pbf 1884034
   ```
   Ghi ra `out/province.geojson` và in số tile cần render.
3. Render và đóng gói:
   ```
   .venv/bin/python province_pack.py data/vietnam-latest.osm.pbf --geojson out/province.geojson --name dak-lak
   ```
   - z14–16 phủ cả tỉnh, z17 quanh các thành phố/thị trấn (`--z17-city-km 4 --z17-town-km 2`, `--no-z17` để bỏ).
   - `--no-night` để bỏ bản đồ đêm, `--workers N` số tiến trình.
   - Đắk Lắk: khoảng 12 phút trên máy 8 nhân, 6.746 file, 447 MB.
4. Kết quả: `out/provinces/dak-lak/nav_map_daklak/`. **Chép nguyên thư mục này vào gốc thẻ SD** (thẻ FAT32). `manifest.json` bên cạnh là danh sách file, kích thước, md5.

## Định dạng gói

Mỗi ô z14 (khoảng 2,4 km) một file `nav_map_<tỉnh>/<day|night>/<x14>/<y14>.pak`:

```
"NPK1", u16 n, u16 0, u32 str_off, u32 str_len
n x {u8 z, u8 rx, u8 ry, u8 0, u32 off, u32 len}     tile z14-17 trong ô
dữ liệu tile: ảnh LVGL 9 RGB565 nén LZ4 (256x256)
tên đường của ô (chỉ gói ngày): khối "STR1" như streets_bin.py
```

Tile trống (toàn màu nền) không lưu; thiết bị tô màu nền.

## Các script

| Script | Việc |
|---|---|
| `render.py` | Đọc OSM, vẽ tile (kiểu ngày; `use_night_style()` cho kiểu đêm). Sửa màu/độ rộng đường trong bảng `ROADS`, `NIGHT_ROADS` |
| `province.py` | Lấy ranh giới tỉnh từ relation id |
| `province_pack.py` | Render cả tỉnh + đóng gói + manifest |
| `pack.py` | Gộp thư mục tile rời `z/x/y.bin` (+ tên đường từ file OSM) thành gói |
| `streets_bin.py` | Xuất `streets.bin` (tên đường nhúng sẵn trong firmware, dùng khi thẻ chưa có gói) |
| `route_bin.py` | Chuyển tuyến OSRM (JSON) sang `route.bin` (tuyến mẫu cho chế độ SIM) |

Dữ liệu © OpenStreetMap contributors (ODbL). Font Be Vietnam Pro (SIL OFL, `fonts/OFL.txt`).
