/*
 * BLE (NimBLE, peripheral "NavESP"): nhận tuyến đường từ điện thoại, báo trạng thái dẫn đường.
 * Giao thức: docs/ble_protocol.md
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum { NAV_IDLE = 0, NAV_NAVIGATING = 1, NAV_OFF_ROUTE = 2, NAV_ARRIVED = 3 };
enum { UPLOAD_NONE = 0, UPLOAD_RECEIVING = 1, UPLOAD_OK = 2, UPLOAD_ERR_CRC = 3, UPLOAD_ERR_FORMAT = 4,
       UPLOAD_ERR_SIZE = 5, UPLOAD_ERR_SEQUENCE = 6 };
enum { NAV_FLAG_GPS_FIX = 1, NAV_FLAG_SIM = 2, NAV_FLAG_ROUTE_LOADED = 4, NAV_FLAG_ROUTE_ACTIVE = 8, NAV_FLAG_NIGHT = 16 };
enum { CMD_START = 1, CMD_STOP = 2, CMD_CLEAR = 3, CMD_SIM_ON = 4, CMD_SIM_OFF = 5, CMD_SIM_OFF_ROUTE = 6, CMD_THEME = 8 };

// Trạng thái gửi lên điện thoại (characteristic status), 29 byte little-endian
typedef struct __attribute__((packed)) {
    uint8_t version;          // 1
    uint8_t state;            // NAV_*
    uint8_t flags;            // NAV_FLAG_*
    uint8_t upload;           // UPLOAD_*: kết quả lần gửi tuyến gần nhất
    int32_t lat_e7, lon_e7;   // vị trí hiện tại
    uint16_t speed_cms;       // tốc độ cm/s
    uint16_t next_step;       // chỉ số bước rẽ tiếp theo trong tuyến, 0xFFFF = không có
    uint32_t dist_next_m;     // khoảng cách tới chỗ rẽ tiếp theo
    uint32_t remain_m;        // quãng đường còn lại
    uint32_t route_crc;       // CRC32 dữ liệu tuyến đang dùng (để app biết thiết bị có đúng tuyến)
    uint8_t reserved;         // dành riêng, luôn = 100 (từng là độ sáng, đã bỏ)
} nav_status_t;

// main.c cung cấp: nạp tuyến mới (đã kiểm CRC) -> true nếu đúng định dạng RTE1; lệnh điều khiển; trạng thái hiện tại
bool nav_route_received(const uint8_t *data, size_t len, uint32_t crc);
void nav_control(uint8_t cmd);
void nav_get_status(nav_status_t *st);
void nav_set_theme(bool night);         // giao diện ngày/đêm, do app quyết định

void ble_nav_init(void);
void ble_nav_status_changed(void);   // báo có thay đổi để gửi notify ngay
bool ble_nav_connected(void);        // app điện thoại đang kết nối
void nav_log_event(const char *msg); // main.c: ghi sự kiện vào log thẻ SD
