# NavESP (iPhone)

App SwiftUI gửi tuyến đường xuống thiết bị qua Bluetooth (giao thức: `../docs/ble_protocol.md`).

## Build và cài lên iPhone

Cần máy Mac có Xcode 16 trở lên và [XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`).

1. Tạo `NavESP/Config/Local.xcconfig` (không đưa vào git):
   ```
   NAVESP_TEAM_ID = <Team ID>
   NAVESP_BUNDLE_ID = com.tenban.navesp
   ```
   Team ID: Xcode → Settings → Accounts → chọn Apple ID → Team (Personal Team cũng được), hoặc trang developer.apple.com → Membership. Bundle id phải là của riêng bạn.
2. Tạo project: `cd NavESP && xcodegen`, rồi mở `NavESP.xcodeproj`.
3. Cắm iPhone, bật **Cài đặt → Quyền riêng tư & Bảo mật → Chế độ nhà phát triển**, chọn iPhone trong Xcode rồi bấm **Run**.
4. Lần đầu: **Cài đặt → Cài đặt chung → Quản lý VPN & thiết bị** → tin cậy Apple ID của bạn. Cho phép Bluetooth, Vị trí, Camera (quét mã QR) khi app hỏi.

Apple ID miễn phí: app hết hạn sau 7 ngày, cắm máy và bấm Run lại.

## Máy chủ chỉ đường mặc định trong bản build (không bắt buộc)

Thêm vào `Local.xcconfig` để app tự dùng máy chủ riêng ngay lần đầu (người dùng vẫn đổi được trong Cài đặt):

```
NAVESP_ROUTE_URL = https:/$()/route.example.com
NAVESP_ROUTE_KEY = <khoá>
```

(`$()` để xcconfig không hiểu `//` là chú thích.) Không đặt thì app dùng máy chủ demo cho tới khi người dùng nhập.

## Mã nguồn

- `BLEManager.swift`: quét, kết nối, tự nối lại, nhận trạng thái, gửi tuyến (BEGIN/DATA/END + CRC32) và lệnh.
- `NavController.swift`: điểm đến, tính đường, gửi tuyến, tự tính lại khi lệch tuyến, kết thúc khi tới nơi, chế độ ngày/đêm.
- `RouteService.swift`, `RouteSettings.swift`: gọi OSRM (máy chủ riêng + dự phòng demo), lưu địa chỉ/khoá (khoá trong Keychain).
- `RouteEncoder.swift`: chuyển kết quả OSRM sang định dạng tuyến RTE1 của thiết bị.
- `SunTimes.swift`: giờ mặt trời mọc/lặn tính tại máy, không cần mạng.
