# Giao thức BLE "NavESP" (thiết bị dẫn đường ↔ điện thoại)

Đây là hợp đồng giữa firmware `firmware/nav` (`main/ble_nav.c`, project ESP-IDF tên `test_nav`) và app iOS. Mọi số đều **little-endian**.

## Quảng bá và kết nối

- Tên quảng bá: **`NavESP`**. Gói quảng bá có UUID dịch vụ bên dưới, nên app nên lọc theo UUID khi quét.
- Thiết bị là BLE peripheral, chỉ nhận **1 kết nối** cùng lúc; mất kết nối thì tự quảng bá lại.
- MTU ưu tiên **247**. iOS thường thoả thuận 185 trở lên. Gói dữ liệu tuyến dùng tối đa `MTU - 3 - 5` byte dữ liệu.
- Không cần ghép đôi (pairing).
- Khởi động: **không có tuyến, không SIM**. Thiết bị chỉ hiện bản đồ và vị trí GPS thật, `state = 0`. Tuyến chỉ có sau khi app gửi qua `route`, hoặc khi bấm SIM để demo.

## UUID

| Thành phần | UUID | Thuộc tính |
|---|---|---|
| Dịch vụ | `4e415600-ba5e-4c0d-8e2f-6a1b3c5d7e90` | |
| `route` | `4e415600-ba5e-4c0d-8e2f-6a1b3c5d7e91` | Write, Write Without Response |
| `status` | `4e415600-ba5e-4c0d-8e2f-6a1b3c5d7e92` | Read, Notify |
| `control` | `4e415600-ba5e-4c0d-8e2f-6a1b3c5d7e93` | Write |

## route: gửi tuyến đường

Dữ liệu tuyến là file **RTE1** (định dạng trong `tools/map_render/route_bin.py`; app iOS tạo đúng định dạng này từ kết quả OSRM). Gửi theo 3 loại gói:

| Gói | Nội dung |
|---|---|
| BEGIN | `u8 1`, `u32 tổng_số_byte`, `u32 crc32` |
| DATA | `u8 2`, `u32 offset`, dữ liệu… (các gói phải nối tiếp: `offset` = số byte đã gửi trước đó) |
| END | `u8 3` |

- CRC32 là chuẩn zlib/IEEE (`zlib.crc32` trong Python, `crc32` với đa thức 0xEDB88320). Tuyến tối đa 256 KB.
- Khi nhận END, thiết bị kiểm tra độ dài, CRC và định dạng RTE1. Nếu đúng thì **thay tuyến đang dùng ngay lập tức**, đặt lại tiến trình về đầu tuyến và vẽ lại bản đồ.
- Kết quả trả về ở trường `upload` của `status`.
- Nên dùng Write (có phản hồi) để chắc chắn thứ tự gói. Write Without Response nhanh hơn nhưng app phải tự chờ khi bộ đệm đầy (`canSendWriteWithoutResponse` trên iOS).
- Mất kết nối giữa chừng thì phần đã nhận bị bỏ, phải gửi lại từ BEGIN.

## status: trạng thái (28 byte)

Thiết bị gửi notify khi `state`, `next_step`, `upload` hoặc `flags` thay đổi, và đều đặn mỗi 2 giây. App cũng có thể đọc trực tiếp bất kỳ lúc nào.

| Offset | Kiểu | Trường | Ý nghĩa |
|---|---|---|---|
| 0 | u8 | version | 1 |
| 1 | u8 | state | 0 idle (không có tuyến/chưa có vị trí), 1 đang dẫn đường, **2 lệch tuyến** (app nên tính lại đường), 3 đã đến nơi |
| 2 | u8 | flags | bit0 có fix GPS, bit1 đang SIM, bit2 đã có tuyến, bit3 tuyến đang bật, bit4 giao diện đêm |
| 3 | u8 | upload | 0 chưa gửi, 1 đang nhận, 2 OK, 3 sai CRC, 4 sai định dạng, 5 quá lớn/hết bộ nhớ, 6 sai thứ tự gói |
| 4 | i32 | lat_e7 | vĩ độ × 10^7 |
| 8 | i32 | lon_e7 | kinh độ × 10^7 |
| 12 | u16 | speed_cms | tốc độ (cm/s) |
| 14 | u16 | next_step | chỉ số bước rẽ tiếp theo trong tuyến RTE1, 0xFFFF = không có |
| 16 | u32 | dist_next_m | khoảng cách tới chỗ rẽ tiếp theo (m) |
| 20 | u32 | remain_m | quãng đường còn lại (m) |
| 24 | u32 | route_crc | CRC32 tuyến đang dùng (0 nếu không có), để app biết thiết bị có đúng tuyến của mình |
| 28 | u8 | reserved | dành riêng, luôn = 100. Từng là độ sáng, đã bỏ vì PWM đèn nền làm màn hình chớp và giảm số vệ tinh. App chỉ cần đọc 28 byte đầu |

Đến nơi: còn dưới 30 m theo tuyến (`ARRIVE_M` trong firmware; với GPS thật, tuyến phải bật hơn 20 giây) thì thiết bị chốt đến nơi: `state = 3`, thôi báo lệch tuyến, đang SIM thì tắt SIM. Màn hình báo "Đã đến nơi" 10 giây rồi gỡ tuyến: bit2 và bit3 của `flags` về 0, `route_crc = 0`. `state` vẫn giữ 3 sau khi gỡ, để app kết nối lại vẫn biết chuyến đã xong, tới khi có tuyến mới hoặc lệnh 1, 2, 3, 4, 5 thì về 0. App gặp `state = 3` thì kết thúc dẫn đường, không gửi lại tuyến và không tính lại đường. Khung 28 byte không đổi. App cũ không xử lý `state = 3` sẽ thấy thiết bị mất tuyến và gửi lại tuyến.

Lệch tuyến: xe cách tuyến hơn 40 m liên tục 3 giây. App nhận `state = 2` thì lấy `lat_e7`/`lon_e7` (hoặc GPS của iPhone) làm điểm xuất phát, tính lại đường tới đích và gửi tuyến mới.

## control: lệnh (1 byte)

| Byte | Lệnh |
|---|---|
| 1 | Bắt đầu dẫn đường theo tuyến đã có |
| 2 | Dừng dẫn đường (giữ tuyến) |
| 3 | Xoá tuyến (đang SIM thì tắt SIM luôn); `status` về `state = 0` |
| 4 | Bật SIM. Đây là cách duy nhất để bật SIM: thiết bị không còn nút SIM |
| 5 | Tắt SIM |
| 6 | Thử lệch tuyến: SIM rời tuyến, đi thẳng ~400 m theo hướng hiện tại rồi đứng chờ (chỉ khi đang SIM). App sẽ thấy `state = 2` và tự tính lại đường; tuyến mới gửi xuống thì SIM chạy theo tuyến mới |
| 7 | Không làm gì. Từng là lệnh đặt độ sáng, đã bỏ: đèn nền luôn sáng 100% |
| 8 | Đặt giao diện, **2 byte**: `08 00` ngày, `08 01` đêm. Thiết bị lưu lại, khởi động vẫn giữ tới khi app gửi lệnh khác. Thiết bị không tự đổi: giờ mặt trời mọc/lặn do app tính. Đêm dùng tile `/tiles_min_lz4_night`, thiếu file thì dùng tile ngày |

Chế độ SIM (chạy giả lập 10 Hz thay cho GPS thật):
- **Đã có tuyến từ app:** chạy dọc tuyến đó. Tắt SIM thì **tuyến vẫn giữ** để dẫn đường bằng GPS thật.
- **Chưa có tuyến:** thiết bị nạp **tuyến mẫu** nhúng sẵn để demo. Tắt SIM thì tuyến mẫu bị gỡ, trở về bản đồ không tuyến.
- App gửi tuyến mới trong lúc SIM đang chạy (kể cả trên tuyến mẫu): thiết bị chuyển sang tuyến mới và chạy lại SIM từ đầu tuyến đó.
- Trong lúc SIM, `flags` bit1 = 1 và vị trí trong `status` là vị trí giả lập.

## Thử nhanh

### Trên iPhone bằng nRF Connect (hoặc LightBlue)

1. Mở nRF Connect, tab Scanner, kết nối **NavESP**.
2. Mở dịch vụ `…7E90`:
   - `…7E92` (status): bấm nút **mũi tên xuống** để đọc, hoặc **ba mũi tên** để bật notify. Giá trị hiện dạng hex 28 byte theo bảng trên. Ví dụ byte thứ 2 (`state`) = `01` là đang dẫn đường.
   - `…7E93` (control): bấm **mũi tên lên**, chọn kiểu *Byte array*, nhập `04` rồi gửi: thiết bị bật SIM. Nhập `02` để dừng dẫn đường, `01` để bật lại.
3. Gửi tuyến bằng tay qua `…7E91` rất mất công (nhiều gói), nên dùng script trên Mac hoặc app riêng.

### Trên Mac bằng Python (bleak)

Chạy trong **Terminal.app của macOS**. Lần đầu macOS sẽ hỏi quyền Bluetooth cho Terminal, anh bấm cho phép.

```
cd ~/Work/esp32_navigation/tools/ble_test
.venv/bin/python send_route.py route2.bin --watch 20
```

Script quét NavESP, in trạng thái, gửi tuyến (BEGIN/DATA/END, CRC32) và in các notify. Chạy `--cmd 4` để bật SIM, `--watch 60` để chỉ theo dõi.
