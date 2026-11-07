/*
 * Test dẫn đường: bản đồ tile (LVGL .bin trên thẻ SD) chạy theo vị trí GPS và xoay theo hướng đi
 * (heading-up), có nút chuyển sang hướng bắc ở trên (north-up). Bản north-up cũ ở backup_northup/.
 * 3x3 tile quanh xe được ghép vào 1 ảnh 768x768 trong PSRAM rồi xoay 1 lần (không lộ đường ghép giữa các tile);
 * chỉ xoay lại khi hướng đổi >= ROT_STEP. Bản xoay 9 tile riêng ở backup_rot9/.
 * Tile tối giản không chữ (tools/map_render); tên đường vẽ riêng sau khi xoay nên luôn đứng thẳng,
 * thanh dưới hiện tên đường đang đi. Dữ liệu tên đường (streets.bin) nhúng trong firmware, font tiếng Việt font_vn_16/14.
 * Khởi động: không có tuyến, không SIM (bản đồ + vị trí GPS thật). Nút SIM (đỏ khi bật) chạy giả lập 10 Hz
 * thay cho GPS thật: dọc tuyến nhận qua BLE nếu có, không thì nạp tuyến mẫu nhúng sẵn (route.bin) để demo.
 * Tắt SIM: tuyến mẫu bị gỡ, tuyến nhận qua BLE vẫn giữ để dẫn đường bằng GPS thật.
 * Chỉ đường: tuyến (route.bin, tools/map_render/route_bin.py) vẽ màu xanh vào ảnh ghép nên xoay theo bản đồ;
 * ô chỉ dẫn phía trên: mũi tên rẽ, khoảng cách tới chỗ rẽ, tên đường sẽ rẽ vào, quãng còn lại + thời gian; báo lệch tuyến, đã đến nơi.
 * BLE "NavESP" (ble_nav.c, docs/ble_protocol.md): điện thoại gửi tuyến mới và lệnh, thiết bị báo trạng thái.
 * Ghi log chuyến đi ra thẻ SD: /logs/NAVxxxxx.CSV, mỗi lần khởi động 1 file (dòng P 2 lần/giây, dòng E cho sự kiện).
 * Tự nhận module GPS lúc khởi động: UBX ở 115200 là NEO-M8U, NMEA là NEO-6M (hoặc bản clone: chỉ dùng GGA/RMC/GSV).
 * GPS NEO-M8U: HNR-PVT 10 Hz cho vị trí/tốc độ, NAV-PVT 2 Hz cho số vệ tinh (cấu hình chỉ RAM lúc khởi động).
 * Kéo tay để xem chỗ khác (tạm dừng chạy theo), nút định vị để quay lại, nút +/- để zoom.
 * Zoom tự đổi theo tốc độ khi đang chạy theo GPS (xem AUTOZOOM_*); bấm +/- thì tạm dừng tự đổi một lúc.
 * Tile được nạp ở task riêng (loader) vào cache LRU trong PSRAM, task LVGL không bao giờ đọc thẻ SD.
 * Màn hình ST7789 ngang 320x240, cảm ứng XPT2046, SD trên SPI3, GPS trên UART1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include "esp_system.h"
#include <math.h>
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "libs/lz4/lz4.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "ble_nav.h"

#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "test_nav";

// ===== Màn hình =====
#define LCD_HOST      SPI2_HOST
#define PIN_SCK       12
#define PIN_MOSI      11
#define PIN_MISO      13
#define PIN_CS        10
#define PIN_DC        8
#define PIN_RST       9
#define PIN_BL        18
#define LCD_W         320
#define LCD_H         240
#define LCD_PCLK_HZ   (40 * 1000 * 1000)

// ===== Cảm ứng (hiệu chỉnh đã đo) =====
#define PIN_T_CS        17
#define PIN_T_IRQ       16
#define TOUCH_CLK_HZ    (2 * 1000 * 1000)
#define TOUCH_X_RAW_MIN 160      // trục x màn hình lấy từ raw Y
#define TOUCH_X_RAW_MAX 3860
#define TOUCH_Y_RAW_MIN 230      // trục y màn hình lấy từ raw X
#define TOUCH_Y_RAW_MAX 3820

// ===== Thẻ SD =====
#define SD_HOST       SPI3_HOST
#define PIN_SD_SCK    4
#define PIN_SD_MISO   5
#define PIN_SD_MOSI   6
#define PIN_SD_CS     7
#define SD_FREQ_KHZ   20000
#define MOUNT_POINT   "/sdcard"
#define TILE_DIR          "tiles_min_lz4"   // tile tự render, kiểu tối giản (tools/map_render)
#define TILE_DIR_NIGHT    "tiles_min_lz4_night"   // tile tối cho chế độ đêm; thiếu file thì dùng TILE_DIR
#define TILE_DIR_FALLBACK "tiles_lz4"       // tile OSM gốc, dùng khi thiếu tile tự render

// ===== GPS =====
#define GPS_UART      UART_NUM_1
#define PIN_GPS_RX    42
#define PIN_GPS_TX    41
#define GPS_BAUD      115200   // NEO-M8U: đã cấu hình sẵn 115200, UBX
#define GPS_NMEA_BAUD 38400    // module NMEA (NEO-6M): chạy 38400, 5 Hz

// ===== Bản đồ =====
#define TILE          256
#define ZOOM_MIN      14
#define ZOOM_MAX      17
#define START_LAT     12.4892447
#define START_LON     108.0607897
#define START_ZOOM    16
#define MARK_X        (LCD_W / 2)              // vị trí xe trên màn hình (tâm xoay)
#define MARK_Y        (LCD_H * 65 / 100)       // thấp hơn giữa để thấy nhiều đường phía trước
#define COVER_R       224        // khoảng cách xa nhất từ vị trí xe tới góc màn hình (px)
#define GRID_W        3          // hình vuông 2*COVER_R quanh xe nằm gọn trong 3x3 tile ở mọi góc xoay
#define GRID_H        3
#define HEAD_MIN_MPS  1.4f       // dưới ~5 km/h hướng GPS nhiễu: giữ hướng cũ
#define ROT_STEP      25         // chỉ xoay lại bản đồ khi hướng đổi >= 2.5 độ (đơn vị 0.1 độ)
#define MAP_ANTIALIAS 1          // làm mịn khi xoay (0 = nhanh hơn nhưng răng cưa)
#define COMP_W        (GRID_W * TILE)
#define COMP_H        (GRID_H * TILE)
#define GREY_565      0xEF5C     // ô chưa có tile: màu nền bản đồ ngày (236,233,226) dạng RGB565,
#define GREY_NIGHT_565 0x10A3    // ... và đêm (20,22,28). Gói tỉnh bỏ tile trống nên phải trùng màu nền
#define MAX_LABELS    12         // số tên đường tối đa trên màn hình
#define STREET_MAX_M  30         // tên đường đang đi: đoạn đường có tên gần nhất trong phạm vi này
#define NSLOT         (GRID_W * GRID_H)
#define TILE_BYTES    (TILE * TILE * 2)
#define CACHE_N       32         // 32 x 128 KB = 4 MB PSRAM
#define MAX_REQ       ((GRID_W + 2) * (GRID_H + 2))   // ô đang hiện + 1 vòng xung quanh

static spi_device_handle_t touch;
static lv_obj_t *map_img;                  // ảnh ghép 3x3 tile, xoay quanh vị trí xe
static lv_image_dsc_t comp_dsc;
static uint16_t *comp_buf;                 // COMP_W x COMP_H RGB565 trong PSRAM
static int tile_key[NSLOT][4];             // lz4, z, x, y đã ghép vào mỗi ô (-1 = nền xám, -2 = cần vẽ lại)
static lv_obj_t *lbl_status;                // hộp trạng thái góc trên trái (các nhãn con bên dưới)
static lv_obj_t *st_gps, *st_sv, *st_sim, *st_ble, *st_log, *st_fps;
static int zoom = START_ZOOM;
static double cx, cy;                      // vị trí xe (điểm MARK_X, MARK_Y), toạ độ pixel thế giới ở zoom hiện tại
static volatile bool theme_night;          // giao diện đêm; app gửi lệnh, thiết bị không tự đổi
static const bool use_lz4 = true;          // tile LZ4 nhanh hơn RAW ~9 lần
static bool follow = true;                 // bản đồ chạy theo vị trí GPS
static bool heading_up = true;             // true: hướng đi ở trên; false: hướng bắc ở trên
static float disp_heading;                 // hướng đang hiển thị (độ, đã làm mượt)
static int map_rot = -1;                   // góc xoay bản đồ đang đặt (0.1 độ, chiều kim đồng hồ)
static lv_obj_t *lbl_speed, *btn_follow, *lbl_mode, *arrow;
static int fps_now, frame_avg_ms;          // thống kê 1 giây gần nhất cho nhãn trạng thái
static int64_t refr_start;
static int last_frame_ms;
static bool tiles_changed;                 // có tile mới được nạp từ lần vẽ trước

// ---------- Toạ độ ----------
static void latlon_to_px(double lat, double lon, int z, double *x, double *y)
{
    double n = TILE * pow(2, z);
    double r = lat * M_PI / 180.0;
    *x = (lon + 180.0) / 360.0 * n;
    *y = (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * n;
}

// ---------- GPS (UBX) ----------
typedef struct {
    bool have_pos;
    double lat, lon;       // độ
    float speed_mps;
    float heading;         // độ, hướng di chuyển
    uint8_t fix_type;      // 0 none, 1 DR, 2 2D, 3 3D, 4 GNSS+DR, 5 time
    bool fix_ok;
    uint8_t num_sv;
    int64_t t_us;          // lúc nhận HNR-PVT, để nội suy vị trí giữa 2 lần cập nhật
    float h_acc_m;         // sai số ngang ước tính
    float s_acc_mps;       // sai số tốc độ ước tính (HNR-PVT sAcc); 0 = không biết (module NMEA)
    uint16_t year;         // giờ UTC theo HNR-PVT
    uint8_t month, day, hour, min, sec;
    uint8_t sv_view;       // module NMEA: số vệ tinh nhìn thấy và C/N0 theo GSV
    uint8_t cno_max;
    float cno_mean;
} gps_data_t;

static gps_data_t gps;
static portMUX_TYPE gps_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t hnr_count;
static float gps_interp_s = 0.2f;          // nội suy vị trí tối đa bấy lâu sau lần cập nhật cuối

static uint32_t u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static int32_t i32(const uint8_t *p) { return (int32_t)u32(p); }

// UBX-NAV-PVT (01 07, 2 Hz): chỉ lấy số vệ tinh và cờ fix
static void on_nav_pvt(const uint8_t *p, int len)
{
    if (len < 92) return;
    taskENTER_CRITICAL(&gps_lock);
    gps.fix_ok = p[21] & 0x01;
    gps.num_sv = p[23];
    taskEXIT_CRITICAL(&gps_lock);
}

// UBX-HNR-PVT (28 00, 10 Hz): vị trí, tốc độ, hướng
static void on_hnr_pvt(const uint8_t *p, int len)
{
    if (len < 72) return;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&gps_lock);
    hnr_count++;
    gps.fix_type = p[16];
    gps.year = p[4] | (p[5] << 8);
    gps.month = p[6]; gps.day = p[7]; gps.hour = p[8]; gps.min = p[9]; gps.sec = p[10];
    gps.h_acc_m = ((uint32_t)p[52] | (p[53] << 8) | (p[54] << 16) | ((uint32_t)p[55] << 24)) / 1000.0f;
    gps.s_acc_mps = u32(p + 60) / 1000.0f;
    if (gps.fix_type >= 2 && gps.fix_type <= 4) {
        gps.have_pos = true;
        gps.lon = i32(p + 20) * 1e-7;
        gps.lat = i32(p + 24) * 1e-7;
        gps.speed_mps = i32(p + 36) / 1000.0f;    // gSpeed mm/s
        gps.heading = i32(p + 44) * 1e-5f;        // headMot
        gps.t_us = now;
    }
    taskEXIT_CRITICAL(&gps_lock);
}

// Lệnh cấu hình đang chờ trả lời: 0 chờ, 1 ACK, -1 NAK
static uint8_t cfg_wait_cls, cfg_wait_id;
static volatile int cfg_result;

static uint8_t ubx[1024];
static volatile uint32_t gps_ubx_frames, gps_nmea_ok;
static int ubx_len, ubx_need;

static void ubx_feed(uint8_t c)
{
    if (ubx_len == 0 && c != 0xB5) return;
    if (ubx_len == 1 && c != 0x62) { ubx_len = 0; return; }
    ubx[ubx_len++] = c;
    if (ubx_len == 6) {
        ubx_need = 8 + (ubx[4] | (ubx[5] << 8));
        if (ubx_need > (int)sizeof(ubx)) { ubx_len = 0; return; }
    }
    if (ubx_len >= 6 && ubx_len == ubx_need) {
        uint8_t a = 0, b = 0;
        for (int i = 2; i < ubx_need - 2; i++) { a += ubx[i]; b += a; }
        if (a == ubx[ubx_need - 2] && b == ubx[ubx_need - 1]) {
            const uint8_t *p = ubx + 6;
            int len = ubx_need - 8;
            gps_ubx_frames++;
            if (ubx[2] == 0x28 && ubx[3] == 0x00) on_hnr_pvt(p, len);
            if (ubx[2] == 0x01 && ubx[3] == 0x07) on_nav_pvt(p, len);
            if (ubx[2] == 0x05 && ubx[3] <= 0x01 && len >= 2 && p[0] == cfg_wait_cls && p[1] == cfg_wait_id) {
                cfg_result = ubx[3] == 0x01 ? 1 : -1;
            }
        }
        ubx_len = 0;
    }
}

// ---------- Module NMEA (NEO-6M) ----------
static void log_event(const char *fmt, ...);

static double nmea_coord(const char *f, const char *hemi)
{
    double v = strtod(f, NULL);
    int deg = (int)(v / 100);
    double d = deg + (v - deg * 100) / 60.0;
    return (hemi[0] == 'S' || hemi[0] == 'W') ? -d : d;
}

// $--GGA: chất lượng fix, số vệ tinh dùng, HDOP. Sai số ngang chỉ là ước lượng (HDOP x 2.5 m), NMEA không có hAcc.
static void on_gga(char **f, int n)
{
    if (n < 9) return;
    int q = atoi(f[6]), sv = atoi(f[7]);
    taskENTER_CRITICAL(&gps_lock);
    gps.fix_ok = q > 0;
    gps.fix_type = q > 0 ? (sv >= 4 ? 3 : 2) : 0;
    gps.num_sv = sv;
    gps.h_acc_m = f[8][0] ? strtof(f[8], NULL) * 2.5f : 0;
    taskEXIT_CRITICAL(&gps_lock);
}

// $--RMC: vị trí, tốc độ, hướng, giờ UTC
static void on_rmc(char **f, int n)
{
    if (n < 10) return;
    int64_t now = esp_timer_get_time();
    bool valid = f[2][0] == 'A' && f[3][0] && f[5][0];
    double lat = valid ? nmea_coord(f[3], f[4]) : 0, lon = valid ? nmea_coord(f[5], f[6]) : 0;
    int t = atoi(f[1]), d = atoi(f[9]);
    taskENTER_CRITICAL(&gps_lock);
    hnr_count++;
    if (d > 0) {
        gps.year = 2000 + d % 100; gps.month = d / 100 % 100; gps.day = d / 10000;
        gps.hour = t / 10000; gps.min = t / 100 % 100; gps.sec = t % 100;
    }
    if (valid) {
        gps.have_pos = true;
        gps.lat = lat;
        gps.lon = lon;
        gps.speed_mps = strtof(f[7], NULL) * 0.514444f;
        if (f[8][0]) gps.heading = strtof(f[8], NULL);
        gps.t_us = now;
    }
    taskEXIT_CRITICAL(&gps_lock);
}

// $--GSV theo từng hệ vệ tinh: gộp các câu trong một nhóm, xong nhóm thì tính lại C/N0 chung
enum { GSV_TALKERS = 5 };
static struct { int view, sum, cnt, max, t_sum, t_cnt, t_max; } gsv[GSV_TALKERS];

static void on_gsv(const char *talker, char **f, int n)
{
    if (n < 4) return;
    static const char NAMES[GSV_TALKERS][3] = { "GP", "GL", "GA", "BD", "GB" };
    int k = 0;
    while (k < GSV_TALKERS && strncmp(NAMES[k], talker, 2) != 0) k++;
    if (k == GSV_TALKERS) return;
    int total = atoi(f[1]), num = atoi(f[2]);
    if (num == 1) gsv[k].t_sum = gsv[k].t_cnt = gsv[k].t_max = 0;
    for (int i = 7; i < n; i += 4) {
        if (!f[i][0]) continue;
        int c = atoi(f[i]);
        if (c <= 0) continue;
        gsv[k].t_sum += c;
        gsv[k].t_cnt++;
        if (c > gsv[k].t_max) gsv[k].t_max = c;
    }
    if (num != total) return;
    gsv[k].view = atoi(f[3]);
    gsv[k].sum = gsv[k].t_sum;
    gsv[k].cnt = gsv[k].t_cnt;
    gsv[k].max = gsv[k].t_max;
    int view = 0, sum = 0, cnt = 0, max = 0;
    for (int i = 0; i < GSV_TALKERS; i++) {
        view += gsv[i].view; sum += gsv[i].sum; cnt += gsv[i].cnt;
        if (gsv[i].max > max) max = gsv[i].max;
    }
    taskENTER_CRITICAL(&gps_lock);
    gps.sv_view = view;
    gps.cno_max = max;
    gps.cno_mean = cnt ? (float)sum / cnt : 0;
    taskEXIT_CRITICAL(&gps_lock);
}

static void nmea_feed(uint8_t c)
{
    static char line[100];
    static int len = -1;
    if (c == '$') { len = 0; return; }
    if (len < 0) return;
    if (c != '\r' && c != '\n') {
        if (len < (int)sizeof(line) - 1) line[len++] = c;
        else len = -1;
        return;
    }
    line[len] = 0;
    len = -1;
    char *star = strrchr(line, '*');
    if (!star || line[0] != 'G') return;
    uint8_t sum = 0;
    for (char *p = line; p < star; p++) sum ^= *p;
    if (sum != (uint8_t)strtoul(star + 1, NULL, 16)) return;
    *star = 0;
    gps_nmea_ok++;
    char *f[24];
    int n = 0;
    for (char *p = line; p && n < 24; ) {
        f[n++] = p;
        p = strchr(p, ',');
        if (p) *p++ = 0;
    }
    const char *type = f[0] + 2;
    if (strcmp(type, "GGA") == 0) on_gga(f, n);
    else if (strcmp(type, "RMC") == 0) on_rmc(f, n);
    else if (strcmp(type, "GSV") == 0) on_gsv(f[0], f, n);
}

static void gps_poll_uart(TickType_t timeout)
{
    uint8_t buf[512];
    int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), timeout);
    for (int i = 0; i < n; i++) {
        ubx_feed(buf[i]);
        nmea_feed(buf[i]);
    }
}

// Gửi 1 lệnh UBX-CFG và chờ ACK/NAK (tối đa 3 lần, mỗi lần 1 giây)
static void ubx_cfg(const char *name, uint8_t cls, uint8_t id, const uint8_t *payload, int len)
{
    uint8_t msg[48] = { 0xB5, 0x62, cls, id, len, 0 };
    memcpy(msg + 6, payload, len);
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6 + len; i++) { a += msg[i]; b += a; }
    msg[6 + len] = a;
    msg[7 + len] = b;

    cfg_wait_cls = cls;
    cfg_wait_id = id;
    for (int attempt = 0; attempt < 3; attempt++) {
        cfg_result = 0;
        uart_write_bytes(GPS_UART, msg, 8 + len);
        TickType_t t0 = xTaskGetTickCount();
        while (cfg_result == 0 && xTaskGetTickCount() - t0 < pdMS_TO_TICKS(1000)) gps_poll_uart(pdMS_TO_TICKS(20));
        if (cfg_result != 0) break;
    }
    ESP_LOGI(TAG, "CFG %s: %s", name, cfg_result > 0 ? "ACK" : cfg_result < 0 ? "NAK" : "khong tra loi");
}

static void gps_config_m8u(void)
{
    // Cấu hình chỉ trong RAM (không lưu vào module): nghiệm chuẩn 2 Hz, HNR-PVT 10 Hz
    static const uint8_t RATE_2HZ[] = { 0xF4, 0x01, 0x01, 0x00, 0x01, 0x00 };   // measRate 500 ms, navRate 1, timeRef GPS
    static const uint8_t HNR_10HZ[] = { 10, 0, 0, 0 };
    static const uint8_t MSG_HNR[]  = { 0x28, 0x00, 1 };
    ubx_cfg("RATE 2 Hz", 0x06, 0x08, RATE_2HZ, sizeof(RATE_2HZ));
    ubx_cfg("HNR 10 Hz", 0x06, 0x5C, HNR_10HZ, sizeof(HNR_10HZ));
    ubx_cfg("MSG HNR-PVT", 0x06, 0x01, MSG_HNR, sizeof(MSG_HNR));
}

static void ubx_send_prt(uint32_t baud)
{
    uint8_t m[28] = { 0xB5, 0x62, 0x06, 0x00, 20, 0, 1, 0, 0, 0, 0xD0, 0x08, 0, 0,
                      baud & 0xFF, (baud >> 8) & 0xFF, (baud >> 16) & 0xFF, baud >> 24, 7, 0, 2, 0 };
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 26; i++) { a += m[i]; b += a; }
    m[26] = a;
    m[27] = b;
    uart_write_bytes(GPS_UART, m, sizeof(m));
    uart_wait_tx_done(GPS_UART, pdMS_TO_TICKS(200));
}

// Cấu hình chỉ trong RAM: 38400, 5 Hz, chế độ automotive, chỉ các câu cần dùng (GSV 1 lần/giây để lấy C/N0)
static void gps_config_nmea(int baud)
{
    gps_interp_s = 0.4f;
    if (baud != GPS_NMEA_BAUD) {
        ubx_send_prt(GPS_NMEA_BAUD);
        vTaskDelay(pdMS_TO_TICKS(100));
        uart_set_baudrate(GPS_UART, GPS_NMEA_BAUD);
    }
    static const uint8_t RATE_5HZ[] = { 0xC8, 0x00, 0x01, 0x00, 0x01, 0x00 };
    static const uint8_t NAV5[36] = { 0x01, 0x00, 4 };   // mask: chỉ dynModel; 4 = automotive
    static const uint8_t MSGS[][3] = { { 0xF0, 0x00, 1 }, { 0xF0, 0x04, 1 }, { 0xF0, 0x03, 5 },   // GGA, RMC, GSV
                                       { 0xF0, 0x01, 0 }, { 0xF0, 0x02, 0 }, { 0xF0, 0x05, 0 }, { 0xF0, 0x08, 0 } };   // tắt GLL, GSA, VTG, ZDA
    ubx_cfg("RATE 5 Hz", 0x06, 0x08, RATE_5HZ, sizeof(RATE_5HZ));
    ubx_cfg("NAV5 automotive", 0x06, 0x24, NAV5, sizeof(NAV5));
    for (size_t i = 0; i < sizeof(MSGS) / sizeof(MSGS[0]); i++) {
        char name[16];
        snprintf(name, sizeof(name), "MSG F0-%02X", MSGS[i][1]);
        ubx_cfg(name, 0x06, 0x01, MSGS[i], 3);
    }
}

// Tự nhận module: nghe lần lượt từng baud, thấy gói UBX hợp lệ là M8U, thấy câu NMEA hợp lệ là module NMEA
static void gps_task(void *arg)
{
    static const int BAUDS[] = { GPS_BAUD, GPS_NMEA_BAUD, 9600 };
    bool found = false;
    for (int k = 0; !found; k = (k + 1) % 3) {
        uart_set_baudrate(GPS_UART, BAUDS[k]);
        uart_flush_input(GPS_UART);
        gps_ubx_frames = gps_nmea_ok = 0;
        TickType_t t0 = xTaskGetTickCount();
        while (!found && xTaskGetTickCount() - t0 < pdMS_TO_TICKS(1500)) {
            gps_poll_uart(pdMS_TO_TICKS(50));
            if (gps_nmea_ok >= 2) {
                log_event("GPS module NMEA, %d baud", BAUDS[k]);
                gps_config_nmea(BAUDS[k]);
                found = true;
            } else if (gps_ubx_frames >= 2) {
                log_event("GPS module UBX, %d baud", BAUDS[k]);
                gps_config_m8u();
                found = true;
            }
        }
    }
    while (1) gps_poll_uart(pdMS_TO_TICKS(50));
}

static void gps_init(void)
{
    uart_config_t cfg = {
        .baud_rate = GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART, 8192, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART, PIN_GPS_TX, PIN_GPS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(gps_task, "gps", 6144, NULL, 5, NULL);
}

// ---------- Chuyến đi giả lập (SIM) ----------
static bool sim;
static int64_t sim_t0;
static bool sim_restart;                   // báo phần chỉ đường đặt lại tiến trình khi SIM chạy lại từ đầu

typedef struct { float lat, lon, speed, heading; } simpt_t;
static simpt_t *sim_trk;                   // hành trình giả lập dọc tuyến đang có
static int sim_trk_n;

// Thay dữ liệu GPS bằng điểm giả lập theo thời gian (giống HNR-PVT 10 Hz, lặp lại khi hết)
static void sim_fill(gps_data_t *g, int64_t now)
{
    if (sim_trk && sim_trk_n > 0) {
        int64_t step = (now - sim_t0) / 100000;
        if (step % sim_trk_n == 0) sim_restart = true;
        const simpt_t *p = &sim_trk[step % sim_trk_n];
        g->have_pos = true;
        g->lat = p->lat;
        g->lon = p->lon;
        g->speed_mps = p->speed;
        g->heading = p->heading;
        g->fix_type = 3;
        g->fix_ok = true;
        g->t_us = sim_t0 + step * 100000;
        return;
    }
    // không có hành trình (không có tuyến): giữ nguyên dữ liệu GPS thật
}

// ---------- Ghi log chuyến đi ra thẻ SD ----------
// /logs/NAVxxxxx.CSV: dòng "P" (2 lần/giây) và "E" (sự kiện). Task riêng ưu tiên thấp ghi qua hàng đợi,
// fsync mỗi 2 giây; thẻ bị rút/lỗi thì đóng file và mở file mới khi thẻ đọc lại được.
#define LOG_LINE_MAX  224
#define LOG_SYNC_MS   2000
static QueueHandle_t log_q;
static volatile bool log_active;           // đang ghi được vào thẻ (hiện chấm nhỏ trên thanh trạng thái)
static volatile uint32_t log_drops;

static void log_put(const char *line)
{
    if (log_q && xQueueSend(log_q, line, 0) != pdTRUE) log_drops++;
}

// Ghi 1 dòng sự kiện (gọi được từ mọi task)
static void log_event(const char *fmt, ...)
{
    char msg[160], line[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    for (char *c = msg; *c; c++) if (*c == ',' || *c == '"') *c = ';';     // giữ đúng cột CSV
    snprintf(line, sizeof(line), "E,%lld,,,,,,,,,,,,,,,,,,,,,,,,,,%s\n", esp_timer_get_time() / 1000, msg);
    log_put(line);
}

void nav_log_event(const char *msg) { log_event("%s", msg); }

static volatile bool sd_ok;                 // thẻ đang mount và đọc được (task loader cập nhật)

static FILE *log_open(void)
{
    mkdir("/sdcard/logs", 0775);
    char path[48];
    struct stat st;
    for (int i = 1; i < 100000; i++) {
        snprintf(path, sizeof(path), "/sdcard/logs/NAV%05d.CSV", i);
        if (stat(path, &st) != 0) {
            FILE *f = fopen(path, "w");
            if (!f) return NULL;
            setvbuf(f, NULL, _IOFBF, 8192);
            fputs("type,ms,utc,fix,sv,hacc_m,lat,lon,kmh,heading,sim,ble,route,route_crc,state,seg,dist_route_m,"
                  "next_step,dist_next_m,remain_m,fps,heap_kb,psram_kb,sv_view,cno_max,cno_mean,street,event\n", f);
            ESP_LOGI(TAG, "Ghi log vao %s", path);
            return f;
        }
    }
    return NULL;
}

static void logger_task(void *arg)
{
    static char line[LOG_LINE_MAX];
    static const char *RESET[] = { "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT", "TASK_WDT", "WDT",
                                   "DEEPSLEEP", "BROWNOUT", "SDIO", "USB", "JTAG", "EFUSE", "PWR_GLITCH", "CPU_LOCKUP" };
    FILE *f = NULL;
    bool boot_logged = false;
    TickType_t last_sync = xTaskGetTickCount();
    while (1) {
        bool got = xQueueReceive(log_q, line, pdMS_TO_TICKS(500)) == pdTRUE;
        if (!f && sd_ok) {
            f = log_open();
            if (f && !boot_logged) {
                esp_reset_reason_t r = esp_reset_reason();
                fprintf(f, "E,%lld,,,,,,,,,,,,,,,,,,,,,,,khoi dong; ly do reset %s\n", esp_timer_get_time() / 1000,
                        r < sizeof(RESET) / sizeof(RESET[0]) ? RESET[r] : "?");
                boot_logged = true;
            }
        }
        if (got && f && fputs(line, f) < 0) {
            fclose(f);                                     // thẻ lỗi/bị rút: mở file mới khi thẻ đọc lại được
            f = NULL;
        }
        if (f && xTaskGetTickCount() - last_sync > pdMS_TO_TICKS(LOG_SYNC_MS)) {
            if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
                fclose(f);
                f = NULL;
            }
            last_sync = xTaskGetTickCount();
        }
        log_active = f != NULL;
    }
}

static void log_init(void)
{
    log_q = xQueueCreate(48, LOG_LINE_MAX);
    xTaskCreatePinnedToCore(logger_task, "logger", 4096, NULL, 2, NULL, 1);
}

// ---------- Cache tile trong PSRAM ----------
enum { T_FREE, T_LOADING, T_READY, T_EMPTY };   // EMPTY = không có file, khỏi thử lại

typedef struct {
    int key[4];             // lz4, z, x, y
    int state;
    uint32_t last_use;
    lv_image_dsc_t dsc;     // trỏ vào buf, cho lv_image_set_src
    uint8_t *buf;           // 256x256 RGB565
} tile_t;

static tile_t cache[CACHE_N];
static SemaphoreHandle_t cache_mtx;        // bảo vệ cache[], req[], vis[]
static uint32_t use_tick;
static int req[MAX_REQ][4], req_n;         // hàng đợi tile cần nạp, ưu tiên từ đầu
static int vis[NSLOT][4], vis_n;           // tile đang hiện, không được thay
static TaskHandle_t loader_handle;
static volatile bool tiles_ready;          // loader báo có tile mới, timer LVGL vẽ lại
static volatile int last_load_ms;
static bool sd_ok_shown = true;            // trạng thái SD đang hiện trên nhãn (task LVGL)

static bool sd_try_mount(void);
static void sd_unmount(void);

static bool key_eq(const int *a, const int *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

static tile_t *cache_find(const int *key)
{
    for (int i = 0; i < CACHE_N; i++) {
        if (cache[i].state != T_FREE && key_eq(cache[i].key, key)) return &cache[i];
    }
    return NULL;
}

// Chọn ô cache để nạp tile mới: ô trống, nếu hết thì ô lâu nhất không dùng và không đang hiện
static tile_t *cache_victim(void)
{
    tile_t *best = NULL;
    for (int i = 0; i < CACHE_N; i++) {
        tile_t *t = &cache[i];
        if (t->state == T_FREE) return t;
        if (t->state == T_LOADING) continue;
        bool visible = false;
        for (int v = 0; v < vis_n && !visible; v++) visible = key_eq(vis[v], t->key);
        if (!visible && (!best || t->last_use < best->last_use)) best = t;
    }
    return best;
}

// ---------- Task nạp tile (chỉ task này đọc thẻ SD) ----------
enum { LOAD_OK, LOAD_MISSING, LOAD_ERROR };   // MISSING: không có file/file hỏng; ERROR: lỗi đọc thẻ

// ---------- Gói bản đồ /navmap (xem tools/map_render/pack.py) ----------
// Mỗi tỉnh một thư mục ở gốc thẻ: /nav_map_<tỉnh> (ví dụ /nav_map_daklak), thư mục cũ /navmap vẫn đọc.
// Trong đó mỗi ô z14 một file <day|night>/<x14>/<y14>.pak: tile z14-17 trong ô và tên đường của ô (chỉ gói ngày).
// "NPK1", u16 n, u16 0, u32 str_off, u32 str_len, n x {u8 z, u8 rx, u8 ry, u8 0, u32 off, u32 len}, rồi dữ liệu.
// Mỗi tile trong gói giống hệt một file .bin rời. Không có /navmap thì dùng thư mục tile rời như cũ.
#define NAVMAP_MAX_ROOTS 8         // số thư mục bản đồ (tỉnh) tối đa trên thẻ
#define PACK_SLOTS  5            // gói đang mở (FATFS max_files 8: còn chỗ cho file log và tile rời)
typedef struct {
    int set, x, y;               // set 0 ngày, 1 đêm; -1 = ô trống
    bool missing;                // không có file gói cho ô này
    FILE *f;
    uint16_t n;
    uint32_t str_off, str_len;
    uint8_t *idx;                // n x 12 byte
    uint32_t last;
} pack_t;
static pack_t packs[PACK_SLOTS] = { [0 ... PACK_SLOTS - 1] = { .set = -1 } };
static uint32_t pack_tick;
static int navmap_state = -1;    // -1 chưa kiểm, 0 không có, 1 có
static char navmap_roots[NAVMAP_MAX_ROOTS][32];
static int navmap_n;

// Tìm các thư mục bản đồ ở gốc thẻ (một lần sau mỗi lần mount)
static bool navmap_present(void)
{
    if (navmap_state < 0) {
        navmap_n = 0;
        DIR *d = opendir(MOUNT_POINT);
        struct dirent *e;
        char list[160] = "";
        while (d && (e = readdir(d)) && navmap_n < NAVMAP_MAX_ROOTS) {
            if (e->d_type != DT_DIR) continue;
            if (strcasecmp(e->d_name, "navmap") != 0 && strncasecmp(e->d_name, "nav_map_", 8) != 0) continue;
            if (strlen(e->d_name) >= sizeof(navmap_roots[0])) continue;
            strcpy(navmap_roots[navmap_n++], e->d_name);
            if (strlen(list) + strlen(e->d_name) + 2 < sizeof(list)) { strcat(list, " "); strcat(list, e->d_name); }
        }
        if (d) closedir(d);
        navmap_state = navmap_n > 0;
        ESP_LOGI(TAG, "Thu muc ban do:%s", navmap_n ? list : " khong co, dung tile roi");
        log_event("thu muc ban do:%s", navmap_n ? list : " khong co");
    }
    return navmap_state == 1;
}

static void pack_close_all(void)
{
    for (int i = 0; i < PACK_SLOTS; i++) {
        if (packs[i].f) fclose(packs[i].f);
        heap_caps_free(packs[i].idx);
        packs[i] = (pack_t){ .set = -1 };
    }
    navmap_state = -1;
}

// Gói của ô (set, x14, y14) từ bộ nhớ đệm, mở nếu chưa có; NULL nếu không có gói
static pack_t *pack_get(int set, int x14, int y14)
{
    pack_t *v = &packs[0];
    for (int i = 0; i < PACK_SLOTS; i++) {
        pack_t *p = &packs[i];
        if (p->set == set && p->x == x14 && p->y == y14) {
            p->last = ++pack_tick;
            return p->missing ? NULL : p;
        }
        if (p->last < v->last) v = p;
    }
    if (v->f) fclose(v->f);
    heap_caps_free(v->idx);
    *v = (pack_t){ .set = set, .x = x14, .y = y14, .last = ++pack_tick };
    char path[320];
    FILE *f = NULL;
    for (int r = 0; r < navmap_n && !f; r++) {         // ô thuộc tỉnh nào thì nằm trong thư mục tỉnh đó
        snprintf(path, sizeof(path), MOUNT_POINT "/%s/%s/%d/%d.pak", navmap_roots[r], set ? "night" : "day", x14, y14);
        f = fopen(path, "rb");
    }
    uint8_t h[16];
    if (!f || fread(h, 1, 16, f) != 16 || memcmp(h, "NPK1", 4) != 0) {
        if (f) fclose(f);
        v->missing = true;
        return NULL;
    }
    v->n = h[4] | (h[5] << 8);
    v->str_off = u32(h + 8);
    v->str_len = u32(h + 12);
    v->idx = heap_caps_malloc(v->n * 12 + 1, MALLOC_CAP_SPIRAM);
    if (!v->idx || fread(v->idx, 12, v->n, f) != v->n) {
        fclose(f);
        heap_caps_free(v->idx);
        v->idx = NULL;
        v->missing = true;
        return NULL;
    }
    v->f = f;
    return v;
}

// Mở tile (z, x, y) trong gói của bộ set; trả FILE* đã đặt ở đầu tile (không được đóng)
static FILE *pack_tile(int set, int z, int x, int y)
{
    if (z < 14) return NULL;
    int sh = z - 14;
    pack_t *p = pack_get(set, x >> sh, y >> sh);
    if (!p) return NULL;
    int rx = x - ((x >> sh) << sh), ry = y - ((y >> sh) << sh);
    for (int i = 0; i < p->n; i++) {
        const uint8_t *e = p->idx + 12 * i;
        if (e[0] == z && e[1] == rx && e[2] == ry) {
            return fseek(p->f, u32(e + 4), SEEK_SET) == 0 ? p->f : NULL;
        }
    }
    return NULL;
}

static int load_tile(const int *key, uint8_t *dst, uint8_t *tmp)
{
    char path[64];
    int64_t t0 = esp_timer_get_time();
    // Ưu tiên tile tự render (TILE_DIR), thiếu file thì dùng tile OSM (TILE_DIR_FALLBACK)
    // key[0]: bit0 = LZ4, bit1 = đêm (đêm: thử TILE_DIR_NIGHT trước)
    const bool lz = key[0] & 1, night = key[0] & 2;
    // Thứ tự: gói đêm, tile rời đêm, gói ngày, tile rời ngày, tile OSM. Tile trong gói: không đóng file gói.
    FILE *f = NULL;
    bool own = true;
    const char *src = "roi";
    bool packs_on = lz && navmap_present();
    if (packs_on && night && (f = pack_tile(1, key[1], key[2], key[3]))) { own = false; src = "goi dem"; }
    if (!f && night) {
        snprintf(path, sizeof(path), MOUNT_POINT "/%s/%d/%d/%d.bin", lz ? TILE_DIR_NIGHT : "tiles", key[1], key[2], key[3]);
        f = fopen(path, "rb");
    }
    if (!f && packs_on && (f = pack_tile(0, key[1], key[2], key[3]))) { own = false; src = "goi ngay"; }
    if (!f) {
        snprintf(path, sizeof(path), MOUNT_POINT "/%s/%d/%d/%d.bin", lz ? TILE_DIR : "tiles", key[1], key[2], key[3]);
        f = fopen(path, "rb");
    }
    if (!f && errno == ENOENT && lz) {
        snprintf(path, sizeof(path), MOUNT_POINT "/%s/%d/%d/%d.bin", TILE_DIR_FALLBACK, key[1], key[2], key[3]);
        f = fopen(path, "rb");
    }
    if (!f) {
        if (errno == ENOENT) return LOAD_MISSING;
        ESP_LOGE(TAG, "tile %d/%d/%d: fopen loi errno %d", key[1], key[2], key[3], errno);
        return LOAD_ERROR;
    }

    int res = LOAD_OK;
    uint8_t hdr[12];
    int64_t t1 = t0, t2 = t0;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        res = LOAD_ERROR;
    } else if (hdr[0] != LV_IMAGE_HEADER_MAGIC || hdr[1] != LV_COLOR_FORMAT_RGB565) {
        res = LOAD_MISSING;
    } else if (lz) {
        // Sau header ảnh là header nén: method, compressed_len, raw_len (uint32 mỗi cái)
        uint32_t ch[3];
        if (fread(ch, 1, sizeof(ch), f) != sizeof(ch)) res = LOAD_ERROR;
        else if (ch[0] != 2 || ch[1] > TILE_BYTES + 1024 || ch[2] != TILE_BYTES) res = LOAD_MISSING;
        else if (fread(tmp, 1, ch[1], f) != ch[1]) res = LOAD_ERROR;
        t1 = esp_timer_get_time();
        if (res == LOAD_OK && LZ4_decompress_safe((const char *)tmp, (char *)dst, ch[1], TILE_BYTES) != TILE_BYTES) {
            res = LOAD_MISSING;
        }
        t2 = esp_timer_get_time();
    } else {
        if (fread(dst, 1, TILE_BYTES, f) != TILE_BYTES) res = LOAD_ERROR;
        t1 = t2 = esp_timer_get_time();
    }
    if (own) fclose(f);
    last_load_ms = (int)((t2 - t0) / 1000);
    ESP_LOGD(TAG, "tile %d/%d/%d %s %s: read %lld ms, decode %lld ms%s", key[1], key[2], key[3], lz ? "LZ4" : "RAW", src,
             (t1 - t0) / 1000, (t2 - t1) / 1000, res == LOAD_OK ? "" : res == LOAD_ERROR ? " SD ERROR" : " BAD FILE");
    return res;
}

// Thẻ lỗi (hoặc bị rút ra): unmount rồi thử mount lại mỗi giây
static void sd_recover(void)
{
    sd_ok = false;
    log_event("loi the SD; mount lai");
    tiles_ready = true;        // để timer LVGL cập nhật nhãn "SD error"
    pack_close_all();
    sd_unmount();
    do {
        vTaskDelay(pdMS_TO_TICKS(1000));
    } while (!sd_try_mount());
    ESP_LOGI(TAG, "SD mount lai OK");
    sd_ok = true;
    tiles_ready = true;        // xếp lại các ô để xin nạp lại tile còn thiếu
}

static volatile int st_want_x = -1, st_want_y = -1;   // ô z14 ở giữa màn hình (layout_tiles đặt)
static int st_have_x = -1, st_have_y = -1;
static void streets_window_load(int x14, int y14);

static void loader_task(void *arg)
{
    uint8_t *tmp = heap_caps_malloc(TILE_BYTES + 1024, MALLOC_CAP_SPIRAM);   // dữ liệu nén đọc từ thẻ
    while (1) {
        if (!sd_ok) sd_recover();    // lúc khởi động không mount được, hoặc thẻ vừa lỗi
        int wx = st_want_x, wy = st_want_y;
        if (wx >= 0 && (wx != st_have_x || wy != st_have_y) && navmap_present()) {
            st_have_x = wx;
            st_have_y = wy;
            streets_window_load(wx, wy);      // tên đường 3x3 ô z14 quanh vị trí, từ các gói ngày
        }
        int key[4];
        tile_t *t = NULL;
        xSemaphoreTake(cache_mtx, portMAX_DELAY);
        while (req_n > 0 && !t) {
            memcpy(key, req[0], sizeof(key));
            memmove(req[0], req[1], (--req_n) * sizeof(req[0]));
            if (cache_find(key)) continue;          // đã có hoặc đang nạp
            t = cache_victim();
            if (!t) break;
            memcpy(t->key, key, sizeof(key));
            t->state = T_LOADING;
        }
        xSemaphoreGive(cache_mtx);

        if (!t) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        int res = load_tile(key, t->buf, tmp);
        xSemaphoreTake(cache_mtx, portMAX_DELAY);
        // Lỗi đọc thẻ không lưu vào cache (sẽ thử lại sau khi mount lại), chỉ file thiếu mới đánh dấu EMPTY
        t->state = res == LOAD_OK ? T_READY : res == LOAD_MISSING ? T_EMPTY : T_FREE;
        t->last_use = ++use_tick;
        xSemaphoreGive(cache_mtx);
        tiles_ready = true;
        if (res == LOAD_ERROR) sd_ok = false;
    }
}

static void cache_init(void)
{
    cache_mtx = xSemaphoreCreateMutex();
    for (int i = 0; i < CACHE_N; i++) {
        tile_t *t = &cache[i];
        t->buf = heap_caps_malloc(TILE_BYTES, MALLOC_CAP_SPIRAM);
        ESP_ERROR_CHECK(t->buf ? ESP_OK : ESP_ERR_NO_MEM);
        t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        t->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        t->dsc.header.w = TILE;
        t->dsc.header.h = TILE;
        t->dsc.header.stride = TILE * 2;
        t->dsc.data = t->buf;
        t->dsc.data_size = TILE_BYTES;
    }
    xTaskCreatePinnedToCore(loader_task, "tiles", 4096, NULL, 3, &loader_handle, 1);
}

// ---------- Đặt lại các tile theo tâm hiện tại ----------
static void update_status(void)
{
    sd_ok_shown = sd_ok;
    taskENTER_CRITICAL(&gps_lock);
    gps_data_t g = gps;
    taskEXIT_CRITICAL(&gps_lock);
    // Biểu tượng GPS theo fix: xanh lá = 3D (hoặc GNSS+DR), cam = 2D, xám = chưa có fix; bên cạnh là số vệ tinh
    bool fix3d = g.fix_ok && (g.fix_type == 3 || g.fix_type == 4), fix2d = g.fix_ok && g.fix_type == 2;
    lv_obj_set_style_text_color(st_gps, lv_color_hex(fix3d ? 0x40E060 : fix2d ? 0xFFA030 : 0x909090), 0);
    lv_label_set_text_fmt(st_sv, "%d", g.num_sv);
    if (sim) lv_obj_remove_flag(st_sim, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(st_sim, LV_OBJ_FLAG_HIDDEN);
    // Biểu tượng Bluetooth: xanh khi app iPhone đang kết nối, xám mờ khi không
    lv_obj_set_style_text_color(st_ble, lv_color_hex(ble_nav_connected() ? 0x40A0FF : 0x606060), 0);
    if (log_active) lv_obj_remove_flag(st_log, LV_OBJ_FLAG_HIDDEN);       // đang ghi log ra thẻ
    else lv_obj_add_flag(st_log, LV_OBJ_FLAG_HIDDEN);
    if (!sd_ok_shown) {
        lv_obj_set_style_text_color(st_fps, lv_color_hex(0xFF5050), 0);
        lv_label_set_text(st_fps, "SD error");
    } else {
        lv_obj_set_style_text_color(st_fps, lv_color_hex(0xB0B0B0), 0);
        lv_label_set_text_fmt(st_fps, "%d fps", fps_now);
    }
}

// ---------- Tên đường (streets.bin, xem tools/map_render/streets_bin.py) ----------
LV_FONT_DECLARE(font_vn_16);
LV_FONT_DECLARE(font_vn_14);
LV_FONT_DECLARE(font_vn_12);
extern const uint8_t streets_bin_start[] asm("_binary_streets_bin_start");

typedef struct {
    double mx, my;          // toạ độ Mercator chuẩn hoá 0..1 (nhân TILE*2^z ra pixel thế giới)
    uint16_t name;
    uint8_t prio;
} label_t;

typedef struct {
    uint16_t name;
    uint8_t prio;
    uint16_t n;
    float *pts;             // n cặp lat, lon (độ)
    float lat0, lat1, lon0, lon1;
} road_t;

// Tên đường dùng chung một bảng tên chỉ thêm, không xoá (con trỏ tên luôn còn hiệu lực).
// Nhãn và đoạn đường thì thay cả bộ khi đổi vùng (task LVGL hoặc đang khoá LVGL mới được đọc).
static char **st_names;
static uint16_t *st_name_w;  // độ rộng chữ (px) theo font nhãn, 0 = chưa đo
static int n_names, cap_names;
static int *name_hash;       // bảng băm mở: chỉ số tên + 1, 0 = trống
static int hash_cap;
static label_t *st_labels;
static int n_labels;
static road_t *st_roads;
static int n_roads;
static lv_obj_t *lbl_pool[MAX_LABELS];
static int lbl_pool_name[MAX_LABELS];        // tên đang gán cho mỗi nhãn (-1 = ẩn)
static lv_obj_t *lbl_street;
static bool labels_on = true;
static bool route_on;                       // có tuyến chỉ đường (route.bin)

static int32_t rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint32_t name_hash_of(const uint8_t *p, int len)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// Chỉ số của tên trong bảng chung, thêm nếu chưa có; -1 nếu hết bộ nhớ
static int name_intern(const uint8_t *p, int len)
{
    if (2 * (n_names + 1) > hash_cap) {
        int nc = hash_cap ? hash_cap * 2 : 1024;
        int *nh = heap_caps_calloc(nc, sizeof(int), MALLOC_CAP_SPIRAM);
        if (!nh) return -1;
        for (int i = 0; i < n_names; i++) {
            uint32_t k = name_hash_of((const uint8_t *)st_names[i], strlen(st_names[i])) & (nc - 1);
            while (nh[k]) k = (k + 1) & (nc - 1);
            nh[k] = i + 1;
        }
        heap_caps_free(name_hash);
        name_hash = nh;
        hash_cap = nc;
    }
    uint32_t k = name_hash_of(p, len) & (hash_cap - 1);
    while (name_hash[k]) {
        const char *n = st_names[name_hash[k] - 1];
        if ((int)strlen(n) == len && memcmp(n, p, len) == 0) return name_hash[k] - 1;
        k = (k + 1) & (hash_cap - 1);
    }
    if (n_names == cap_names) {
        int nc = cap_names ? cap_names * 2 : 512;
        char **nn = heap_caps_realloc(st_names, nc * sizeof(char *), MALLOC_CAP_SPIRAM);
        if (!nn) return -1;
        st_names = nn;
        uint16_t *nw = heap_caps_realloc(st_name_w, nc * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!nw) return -1;
        st_name_w = nw;
        cap_names = nc;
    }
    char *c = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!c) return -1;
    memcpy(c, p, len);
    c[len] = 0;
    st_names[n_names] = c;
    st_name_w[n_names] = 0;
    name_hash[k] = n_names + 1;
    return n_names++;
}

static int label_cmp(const void *a, const void *b)
{
    return (int)((const label_t *)a)->prio - (int)((const label_t *)b)->prio;
}

// Ghép các khối "STR1" (streets.bin nhúng sẵn, hoặc phần tên đường trong các gói) thành bộ nhãn/đường
// mới rồi thay bộ cũ. Gọi khi đang giữ khoá LVGL (hoặc trước khi LVGL chạy).
static void streets_merge(const uint8_t *const *chunks, int n_chunks)
{
    int tot_l = 0, tot_r = 0;
    for (int c = 0; c < n_chunks; c++) {               // đếm trước để cấp phát một lần
        const uint8_t *p = chunks[c];
        if (memcmp(p, "STR1", 4) != 0) continue;
        p += 4;
        int nn = rd_u16(p); p += 2;
        for (int i = 0; i < nn; i++) p += 1 + *p;
        int nl = (int)rd_i32(p); p += 4 + nl * 12;
        tot_l += nl;
        tot_r += (int)rd_i32(p);
    }
    label_t *labels = heap_caps_malloc((tot_l + 1) * sizeof(label_t), MALLOC_CAP_SPIRAM);
    road_t *roads = heap_caps_malloc((tot_r + 1) * sizeof(road_t), MALLOC_CAP_SPIRAM);
    if (!labels || !roads) {
        heap_caps_free(labels);
        heap_caps_free(roads);
        ESP_LOGE(TAG, "Ten duong: het bo nho (%d nhan, %d duong)", tot_l, tot_r);
        return;
    }
    int nl_out = 0, nr_out = 0;
    for (int c = 0; c < n_chunks; c++) {
        const uint8_t *p = chunks[c];
        if (memcmp(p, "STR1", 4) != 0) continue;
        p += 4;
        int nn = rd_u16(p); p += 2;
        int *gid = heap_caps_malloc((nn + 1) * sizeof(int), MALLOC_CAP_SPIRAM);
        if (!gid) break;
        for (int i = 0; i < nn; i++) {
            int len = *p++;
            gid[i] = name_intern(p, len);
            p += len;
        }
        int nl = (int)rd_i32(p); p += 4;
        for (int i = 0; i < nl; i++, p += 12) {
            int id = rd_u16(p + 8);
            if (id >= nn || gid[id] < 0 || gid[id] > 0xFFFF) continue;
            double lat = rd_i32(p) * 1e-7, lon = rd_i32(p + 4) * 1e-7;
            label_t *l = &labels[nl_out++];
            l->mx = (lon + 180.0) / 360.0;
            l->my = (1.0 - log(tan(lat * M_PI / 180.0) + 1.0 / cos(lat * M_PI / 180.0)) / M_PI) / 2.0;
            l->name = gid[id];
            l->prio = p[11];
        }
        int nr = (int)rd_i32(p); p += 4;
        for (int i = 0; i < nr; i++) {
            int id = rd_u16(p), npts = rd_u16(p + 4);
            uint8_t prio = p[2];
            p += 6;
            const uint8_t *pts = p;
            p += npts * 8;
            if (id >= nn || gid[id] < 0 || gid[id] > 0xFFFF || npts < 2) continue;
            road_t *r = &roads[nr_out];
            r->pts = heap_caps_malloc(npts * 2 * sizeof(float), MALLOC_CAP_SPIRAM);
            if (!r->pts) continue;
            r->name = gid[id];
            r->prio = prio;
            r->n = npts;
            r->lat0 = r->lon0 = 1e9f;
            r->lat1 = r->lon1 = -1e9f;
            for (int k = 0; k < npts; k++) {
                float la = rd_i32(pts + 8 * k) * 1e-7f, lo = rd_i32(pts + 8 * k + 4) * 1e-7f;
                r->pts[2 * k] = la;
                r->pts[2 * k + 1] = lo;
                r->lat0 = fminf(r->lat0, la); r->lat1 = fmaxf(r->lat1, la);
                r->lon0 = fminf(r->lon0, lo); r->lon1 = fmaxf(r->lon1, lo);
            }
            nr_out++;
        }
        heap_caps_free(gid);
    }
    qsort(labels, nl_out, sizeof(label_t), label_cmp);   // update_labels cần sắp theo priority
    for (int i = 0; i < n_roads; i++) heap_caps_free(st_roads[i].pts);
    heap_caps_free(st_roads);
    heap_caps_free(st_labels);
    st_labels = labels;
    n_labels = nl_out;
    st_roads = roads;
    n_roads = nr_out;
    ESP_LOGI(TAG, "Ten duong: %d khoi, %d ten (tong), %d nhan, %d duong", n_chunks, n_names, n_labels, n_roads);
}

// Lúc khởi động: dữ liệu nhúng sẵn (vùng Buôn Ma Thuột). Có /navmap thì task nạp tile thay bằng dữ liệu trong gói.
static void streets_init(void)
{
    const uint8_t *c = streets_bin_start;
    streets_merge(&c, 1);
}

// Task nạp tile: đọc phần tên đường của 3x3 gói ngày quanh ô (x14, y14) rồi thay bộ đang dùng
static void streets_window_load(int x14, int y14)
{
    uint8_t *bufs[9];
    int n = 0;
    int64_t t0 = esp_timer_get_time();
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            pack_t *p = pack_get(0, x14 + dx, y14 + dy);
            if (!p || p->str_len < 10 || p->str_len > 2 * 1024 * 1024) continue;
            uint8_t *b = heap_caps_malloc(p->str_len, MALLOC_CAP_SPIRAM);
            if (!b) continue;
            if (fseek(p->f, p->str_off, SEEK_SET) != 0 || fread(b, 1, p->str_len, p->f) != p->str_len) {
                heap_caps_free(b);
                continue;
            }
            bufs[n++] = b;
        }
    }
    int64_t t1 = esp_timer_get_time();
    lvgl_port_lock(0);
    streets_merge((const uint8_t *const *)bufs, n);
    lvgl_port_unlock();
    for (int i = 0; i < n; i++) heap_caps_free(bufs[i]);
    ESP_LOGI(TAG, "Ten duong vung %d,%d: %d goi, doc %lld ms, ghep %lld ms", x14, y14, n,
             (t1 - t0) / 1000, (esp_timer_get_time() - t1) / 1000);
}

// Vẽ tên đường đứng thẳng tại vị trí đã xoay theo bản đồ; ưu tiên đường lớn, bỏ nhãn chồng nhau
static void update_labels(void)
{
    int used = 0;
    if (labels_on && st_labels) {
        int max_prio = zoom >= 17 ? 6 : zoom == 16 ? 5 : zoom == 15 ? 4 : 2;   // zoom nhỏ chỉ hiện đường lớn
        double scale = TILE * pow(2, zoom);
        float a = map_rot * (float)M_PI / 1800.0f, ca = cosf(a), sa = sinf(a);
        lv_area_t boxes[MAX_LABELS];
        int names[MAX_LABELS];
        for (int i = 0; i < n_labels && used < MAX_LABELS; i++) {
            const label_t *l = &st_labels[i];
            if (l->prio > max_prio) break;             // nhãn đã sắp theo priority
            float dx = (float)(l->mx * scale - cx), dy = (float)(l->my * scale - cy);
            if (fabsf(dx) > 300 || fabsf(dy) > 300) continue;
            int sx = MARK_X + (int)(dx * ca - dy * sa), sy = MARK_Y + (int)(dx * sa + dy * ca);
            bool dup = false;
            for (int k = 0; k < used && !dup; k++) dup = names[k] == l->name;   // mỗi tên 1 nhãn
            if (dup) continue;
            if (!st_name_w[l->name]) {
                lv_point_t sz;
                lv_text_get_size(&sz, st_names[l->name], &font_vn_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                st_name_w[l->name] = sz.x + 8;
            }
            int w = st_name_w[l->name], h = 24;          // line_height font 14 px (22) + đệm 2*1 px
            lv_area_t b = { sx - w / 2, sy - h / 2, sx + w / 2, sy + h / 2 };
            // trong vùng bản đồ, tránh nút bên phải, ô tốc độ/thanh tên đường bên dưới và mũi tên xe
            if (b.x1 < 2 || b.y1 < (route_on ? 74 : 22) || b.x2 > 252 || b.y2 > 186) continue;   // dưới ô chỉ dẫn/trạng thái
            if (b.x1 < 130 && b.y2 > 150) continue;     // ô tốc độ góc dưới trái
            if (b.x1 < MARK_X + 18 && b.x2 > MARK_X - 18 && b.y1 < MARK_Y + 18 && b.y2 > MARK_Y - 18) continue;
            bool hit = false;
            for (int k = 0; k < used && !hit; k++) {
                hit = b.x1 < boxes[k].x2 + 4 && b.x2 > boxes[k].x1 - 4 && b.y1 < boxes[k].y2 + 2 && b.y2 > boxes[k].y1 - 2;
            }
            if (hit) continue;
            boxes[used] = b;
            names[used] = l->name;
            lv_obj_t *o = lbl_pool[used];
            if (lbl_pool_name[used] != l->name) {
                lv_label_set_text_static(o, st_names[l->name]);
                lbl_pool_name[used] = l->name;
            }
            lv_obj_set_pos(o, b.x1, b.y1);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
            used++;
        }
    }
    for (int k = used; k < MAX_LABELS; k++) {
        if (lbl_pool_name[k] != -1) {
            lv_obj_add_flag(lbl_pool[k], LV_OBJ_FLAG_HIDDEN);
            lbl_pool_name[k] = -1;
        }
    }
}

// Tên đường đang đi: đoạn đường có tên gần nhất (<= STREET_MAX_M), ưu tiên đoạn cùng hướng đi
static const char *current_street(double lat, double lon, float heading, bool moving)
{
    if (!st_roads) return NULL;
    float m_lat = 111320.0f, m_lon = 111320.0f * cosf(lat * M_PI / 180.0);
    float marg = STREET_MAX_M / 111320.0f * 1.5f;
    float best = STREET_MAX_M;
    int best_name = -1;
    for (int i = 0; i < n_roads; i++) {
        const road_t *r = &st_roads[i];
        if (lat < r->lat0 - marg || lat > r->lat1 + marg || lon < r->lon0 - marg || lon > r->lon1 + marg) continue;
        for (int k = 0; k + 1 < r->n; k++) {
            // toạ độ phẳng (m) với gốc tại vị trí xe
            float ax = (r->pts[2 * k + 1] - lon) * m_lon, ay = (r->pts[2 * k] - lat) * m_lat;
            float bx = (r->pts[2 * k + 3] - lon) * m_lon, by = (r->pts[2 * k + 2] - lat) * m_lat;
            float ex = bx - ax, ey = by - ay, len2 = ex * ex + ey * ey;
            float t = len2 > 0 ? -(ax * ex + ay * ey) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float d = hypotf(ax + t * ex, ay + t * ey);
            if (moving && len2 > 1) {          // đoạn lệch hướng đi > 45° (theo 2 chiều) thì coi như xa hơn
                float seg = atan2f(ex, ey) * 180.0f / (float)M_PI;
                float diff = fabsf(fmodf(seg - heading + 540.0f, 360.0f) - 180.0f);
                if (diff > 90) diff = 180 - diff;
                if (diff > 45) d += 20;
            }
            if (d < best) {
                best = d;
                best_name = r->name;
            }
        }
    }
    return best_name >= 0 ? st_names[best_name] : NULL;
}

// ---------- Chỉ đường (route.bin, xem tools/map_render/route_bin.py) ----------
extern const uint8_t route_bin_start[] asm("_binary_route_bin_start");   // tuyến mẫu cho demo SIM
extern const uint8_t route_bin_end[] asm("_binary_route_bin_end");
enum { K_STRAIGHT, K_SLIGHT_RIGHT, K_RIGHT, K_SHARP_RIGHT, K_UTURN, K_SHARP_LEFT, K_LEFT, K_SLIGHT_LEFT,
       K_ROUNDABOUT, K_ARRIVE, K_DEPART };
#define OFF_ROUTE_M     40        // xa tuyến hơn mức này ...
#define OFF_ROUTE_US    3000000   // ... liên tục chừng này thì báo lệch tuyến
#define ARRIVE_M        30        // còn dưới chừng này theo tuyến thì đến nơi
#define ARRIVE_MIN_US   20000000  // tuyến GPS thật mới bật chưa lâu thì chưa chốt (tuyến vòng có điểm cuối sát điểm đầu)
#define ARRIVE_HOLD_US  10000000  // giữ ô "Đã đến nơi" chừng này rồi gỡ tuyến
#define ROUTE_565       0x1B7F    // 0x1E6FFF dạng RGB565
#define ROUTE_EDGE_565  0x09F3    // 0x0A3C99 dạng RGB565

typedef struct {
    double mx, my;          // Mercator chuẩn hoá 0..1
    float lat, lon;
    float cum;              // quãng đường từ đầu tuyến tới điểm này (m)
} rpt_t;

typedef struct {
    int idx;                // điểm tuyến tại chỗ rẽ
    uint8_t kind, exit;
    char name[96];
} rstep_t;

static rpt_t *rt_pts;
static int rt_n;
static rstep_t *rt_steps;
static int rt_nsteps;
static int rt_seg;                         // đoạn tuyến xe đang ở
static uint32_t rt_crc;                    // CRC32 dữ liệu tuyến đang dùng
static bool rt_is_demo;                    // tuyến đang dùng là tuyến mẫu (nạp khi bấm SIM), không phải nhận qua BLE
static nav_status_t nav_st;                // trạng thái dẫn đường cho BLE (task LVGL ghi, task BLE đọc)
static float nav_route_dist = -1;          // khoảng cách từ xe tới tuyến (m), -1 = không có tuyến (ghi log)
static portMUX_TYPE nav_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t rt_t0;             // lúc tuyến bắt đầu dẫn
static int64_t arrive_t;          // lúc chốt đến nơi, 0 = chưa
static bool rt_finished;          // đã đến nơi và gỡ tuyến: giữ state = đã đến nơi tới khi có lệnh/tuyến mới
static void route_ui_refresh(void);
static void sim_stop_keep_route(void);

static void guidance_reset(void)
{
    rt_t0 = esp_timer_get_time();
    arrive_t = 0;
    rt_finished = false;
    taskENTER_CRITICAL(&nav_lock);
    nav_st.state = NAV_IDLE;       // báo ngay, không để app đọc lại trạng thái đến nơi cũ
    taskEXIT_CRITICAL(&nav_lock);
}
static int comp_tx0, comp_ty0;             // tile ở góc trên trái ảnh ghép
static lv_obj_t *turn_panel, *turn_icon, *lbl_turn_dist, *lbl_turn_name, *lbl_remain;
static uint8_t turn_icon_buf[LV_CANVAS_BUF_SIZE(48, 48, 32, LV_DRAW_BUF_STRIDE_ALIGN)];

static float dist_m(float la1, float lo1, float la2, float lo2)
{
    float dy = (la2 - la1) * 111320.0f, dx = (lo2 - lo1) * 111320.0f * cosf(la1 * (float)M_PI / 180.0f);
    return sqrtf(dx * dx + dy * dy);
}

// Đọc tuyến định dạng RTE1 (tools/map_render/route_bin.py) vào mảng mới, kiểm tra kích thước từng phần
static bool route_parse(const uint8_t *p, size_t len, rpt_t **out_pts, int *out_n, rstep_t **out_steps, int *out_ns)
{
    const uint8_t *end = p + len;
    if (len < 10 || memcmp(p, "RTE1", 4) != 0) return false;
    uint32_t n = (uint32_t)rd_i32(p + 4);
    p += 8;
    if (n < 2 || n > 100000 || (size_t)(end - p) < n * 8 + 2) return false;
    rpt_t *pts = heap_caps_malloc(n * sizeof(rpt_t), MALLOC_CAP_SPIRAM);
    if (!pts) return false;
    for (uint32_t i = 0; i < n; i++, p += 8) {
        rpt_t *r = &pts[i];
        double lat = rd_i32(p) * 1e-7, lon = rd_i32(p + 4) * 1e-7;
        r->lat = lat;
        r->lon = lon;
        r->mx = (lon + 180.0) / 360.0;
        r->my = (1.0 - log(tan(lat * M_PI / 180.0) + 1.0 / cos(lat * M_PI / 180.0)) / M_PI) / 2.0;
        r->cum = i ? pts[i - 1].cum + dist_m(pts[i - 1].lat, pts[i - 1].lon, r->lat, r->lon) : 0;
    }
    int ns = rd_u16(p);
    p += 2;
    rstep_t *steps = heap_caps_calloc(ns ? ns : 1, sizeof(rstep_t), MALLOC_CAP_SPIRAM);
    if (!steps) { heap_caps_free(pts); return false; }
    for (int i = 0; i < ns; i++) {
        if (end - p < 7 || end - p < 7 + p[6] || (uint32_t)rd_i32(p) >= n || p[4] > K_DEPART) {
            heap_caps_free(pts);
            heap_caps_free(steps);
            return false;
        }
        rstep_t *st = &steps[i];
        st->idx = (int)rd_i32(p);
        st->kind = p[4];
        st->exit = p[5];
        int l = p[6];
        memcpy(st->name, p + 7, l < (int)sizeof(st->name) - 1 ? l : (int)sizeof(st->name) - 1);
        p += 7 + l;
    }
    *out_pts = pts;
    *out_n = (int)n;
    *out_steps = steps;
    *out_ns = ns;
    return true;
}

// Gỡ tuyến đang dùng (và hành trình SIM theo nó)
static void route_unload(void)
{
    route_on = false;
    guidance_reset();
    rt_is_demo = false;
    heap_caps_free(rt_pts);
    heap_caps_free(rt_steps);
    rt_pts = NULL;
    rt_steps = NULL;
    rt_n = rt_nsteps = 0;
    rt_crc = 0;
}

// Nạp tuyến mẫu nhúng trong firmware để demo SIM khi chưa có tuyến từ điện thoại
static void route_load_demo(void)
{
    size_t len = route_bin_end - route_bin_start;
    route_unload();
    if (!route_parse(route_bin_start, len, &rt_pts, &rt_n, &rt_steps, &rt_nsteps)) {
        ESP_LOGE(TAG, "route.bin sai dinh dang");
        return;
    }
    rt_crc = esp_rom_crc32_le(0, route_bin_start, len);
    rt_is_demo = true;
    route_on = true;
    rt_seg = 0;
    ESP_LOGI(TAG, "Tuyen mau: %d diem, %d buoc, %.0f m", rt_n, rt_nsteps, rt_pts[rt_n - 1].cum);
}

// Tô hình tròn bán kính r vào ảnh ghép (nét vẽ "chổi" cho đường dày)
static void comp_disc(int x, int y, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int row = y + dy;
        if (row < 0 || row >= COMP_H) continue;
        int half = (int)sqrtf((float)(r * r - dy * dy));
        int x0 = x - half < 0 ? 0 : x - half, x1 = x + half >= COMP_W ? COMP_W - 1 : x + half;
        uint16_t *d = comp_buf + row * COMP_W;
        for (int k = x0; k <= x1; k++) d[k] = c;
    }
}

static void comp_line(float x0, float y0, float x1, float y1, int r, uint16_t c)
{
    float len = hypotf(x1 - x0, y1 - y0);
    int n = (int)(len / (r > 2 ? r / 2.0f : 1.0f)) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        comp_disc((int)lroundf(x0 + (x1 - x0) * t), (int)lroundf(y0 + (y1 - y0) * t), r, c);
    }
}

// Vẽ tuyến (viền đậm + lõi xanh) vào ảnh ghép; gọi mỗi khi ảnh ghép được ghép lại
static void draw_route(void)
{
    if (!route_on) return;
    static const int CORE_R[] = { 2, 2, 3, 4 };          // nửa độ rộng lõi theo zoom 14..17
    int r = CORE_R[zoom - ZOOM_MIN];
    double scale = TILE * pow(2, zoom), ox = comp_tx0 * TILE, oy = comp_ty0 * TILE;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i + 1 < rt_n; i++) {
            float x0 = (float)(rt_pts[i].mx * scale - ox), y0 = (float)(rt_pts[i].my * scale - oy);
            float x1 = (float)(rt_pts[i + 1].mx * scale - ox), y1 = (float)(rt_pts[i + 1].my * scale - oy);
            if ((x0 < -8 && x1 < -8) || (y0 < -8 && y1 < -8) || (x0 > COMP_W + 8 && x1 > COMP_W + 8) ||
                (y0 > COMP_H + 8 && y1 > COMP_H + 8)) continue;
            if (pass == 0) comp_line(x0, y0, x1, y1, r + 2, ROUTE_EDGE_565);
            else comp_line(x0, y0, x1, y1, r, ROUTE_565);
        }
    }
}

// Vẽ biểu tượng chỉ dẫn (trắng, 48x48) vào canvas
static void draw_turn_icon(int kind)
{
    lv_canvas_fill_bg(turn_icon, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(turn_icon, &layer);
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = lv_color_white();
    ln.width = 7;
    ln.round_start = ln.round_end = 1;
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.bg_color = lv_color_white();
    tri.bg_opa = LV_OPA_COVER;

    float tipx, tipy, ang;                 // mũi tên: đầu nhọn tại (tipx, tipy), hướng ang (độ, 0 = lên, dương = phải)
    if (kind == K_ARRIVE) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_white();
        arc.center.x = 24; arc.center.y = 20;
        arc.radius = 13; arc.width = 13;
        arc.start_angle = 0; arc.end_angle = 360;
        lv_draw_arc(&layer, &arc);
        tri.p[0].x = 13; tri.p[0].y = 26; tri.p[1].x = 35; tri.p[1].y = 26; tri.p[2].x = 24; tri.p[2].y = 46;
        lv_draw_triangle(&layer, &tri);
        lv_canvas_finish_layer(turn_icon, &layer);
        return;
    }
    if (kind == K_UTURN) {                  // quay đầu bên trái (giao thông bên phải)
        ln.p1.x = 32; ln.p1.y = 44; ln.p2.x = 32; ln.p2.y = 18; lv_draw_line(&layer, &ln);
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_white();
        arc.center.x = 23; arc.center.y = 18; arc.radius = 12; arc.width = 7;
        arc.start_angle = 180; arc.end_angle = 360;
        lv_draw_arc(&layer, &arc);
        ln.p1.x = 14; ln.p1.y = 18; ln.p2.x = 14; ln.p2.y = 28; lv_draw_line(&layer, &ln);
        tipx = 14; tipy = 40; ang = 180;
    } else if (kind == K_ROUNDABOUT) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_white();
        arc.center.x = 24; arc.center.y = 22; arc.radius = 10; arc.width = 5;
        arc.start_angle = 0; arc.end_angle = 360;
        lv_draw_arc(&layer, &arc);
        ln.p1.x = 24; ln.p1.y = 46; ln.p2.x = 24; ln.p2.y = 33; lv_draw_line(&layer, &ln);
        ln.p1.x = 31; ln.p1.y = 15; ln.p2.x = 36; ln.p2.y = 10; lv_draw_line(&layer, &ln);
        tipx = 44; tipy = 2; ang = 45;
    } else {
        static const int ANG[] = { 0, 45, 90, 135, 0, -135, -90, -45 };
        ang = kind <= K_SLIGHT_LEFT ? ANG[kind] : 0;
        float a = ang * (float)M_PI / 180.0f;
        ln.p1.x = 24; ln.p1.y = 46; ln.p2.x = 24; ln.p2.y = 26; lv_draw_line(&layer, &ln);   // thân
        float ex = 24 + 12 * sinf(a), ey = 26 - 12 * cosf(a);
        ln.p1.x = 24; ln.p1.y = 26; ln.p2.x = ex; ln.p2.y = ey; lv_draw_line(&layer, &ln);   // khúc rẽ
        tipx = ex + 13 * sinf(a); tipy = ey - 13 * cosf(a);
    }
    float a = ang * (float)M_PI / 180.0f, bx = tipx - 13 * sinf(a), by = tipy + 13 * cosf(a);
    tri.p[0].x = tipx; tri.p[0].y = tipy;
    tri.p[1].x = bx + 10 * cosf(a); tri.p[1].y = by + 10 * sinf(a);
    tri.p[2].x = bx - 10 * cosf(a); tri.p[2].y = by - 10 * sinf(a);
    lv_draw_triangle(&layer, &tri);
    lv_canvas_finish_layer(turn_icon, &layer);
}

static void fmt_dist(char *buf, int n, float m)
{
    if (m < 1000) snprintf(buf, n, "%d m", (int)(m / 10 + 0.5f) * 10);
    else snprintf(buf, n, "%.1f km", m / 1000.0f);
}

// Cập nhật tiến trình trên tuyến và ô chỉ dẫn (2 lần/giây)
static void update_guidance(const gps_data_t *g, int64_t now)
{
    static int64_t off_since;
    static int shown_kind = -1;
    if (arrive_t) {            // đã chốt đến nơi: không tính lệch tuyến nữa, đi đâu cũng không báo
        if (now - arrive_t <= ARRIVE_HOLD_US) return;
        ESP_LOGI(TAG, "Da den noi: go tuyen");
        log_event("go tuyen sau khi den noi");
        route_unload();
        rt_finished = true;
        off_since = 0;
        taskENTER_CRITICAL(&nav_lock);
        nav_st.state = NAV_ARRIVED;
        nav_st.next_step = 0xFFFF;
        nav_st.dist_next_m = nav_st.remain_m = 0;
        taskEXIT_CRITICAL(&nav_lock);
        route_ui_refresh();
        return;
    }
    if (!route_on || !g->have_pos) {
        lv_obj_add_flag(turn_panel, LV_OBJ_FLAG_HIDDEN);
        nav_route_dist = -1;
        taskENTER_CRITICAL(&nav_lock);
        nav_st.state = rt_finished ? NAV_ARRIVED : NAV_IDLE;
        nav_st.next_step = 0xFFFF;
        nav_st.dist_next_m = nav_st.remain_m = 0;
        nav_st.lat_e7 = g->have_pos ? (int32_t)lround(g->lat * 1e7) : 0;
        nav_st.lon_e7 = g->have_pos ? (int32_t)lround(g->lon * 1e7) : 0;
        nav_st.speed_cms = g->have_pos ? (uint16_t)(g->speed_mps * 100) : 0;
        taskEXIT_CRITICAL(&nav_lock);
        return;
    }
    if (sim_restart) { rt_seg = 0; sim_restart = false; }
    // Gắn vị trí vào tuyến: tìm đoạn gần nhất từ đoạn hiện tại trở đi (không lùi lại); xa quá thì tìm cả tuyến
    float best = 1e9f, best_t = 0;
    int best_i = rt_seg;
    for (int pass = 0; pass < 2 && best > OFF_ROUTE_M; pass++) {
        int from = pass ? 0 : (rt_seg > 0 ? rt_seg - 1 : 0), to = pass ? rt_n - 1 : (rt_seg + 30 < rt_n - 1 ? rt_seg + 30 : rt_n - 1);
        float m_lon = 111320.0f * cosf(g->lat * (float)M_PI / 180.0f);
        for (int i = from; i < to; i++) {
            float ax = (rt_pts[i].lon - g->lon) * m_lon, ay = (rt_pts[i].lat - g->lat) * 111320.0f;
            float bx = (rt_pts[i + 1].lon - g->lon) * m_lon, by = (rt_pts[i + 1].lat - g->lat) * 111320.0f;
            float ex = bx - ax, ey = by - ay, len2 = ex * ex + ey * ey;
            float t = len2 > 0 ? -(ax * ex + ay * ey) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float d = hypotf(ax + t * ex, ay + t * ey);
            if (d < best) { best = d; best_i = i; best_t = t; }
        }
    }
    rt_seg = best_i;
    nav_route_dist = best;
    float prog = rt_pts[best_i].cum + best_t * (rt_pts[best_i + 1].cum - rt_pts[best_i].cum);
    float remain = rt_pts[rt_n - 1].cum - prog;

    if (best > OFF_ROUTE_M) {
        if (!off_since) off_since = now;
    } else {
        off_since = 0;
    }
    bool off = off_since && now - off_since > OFF_ROUTE_US;

    // Chỗ rẽ tiếp theo
    const rstep_t *next = NULL;
    for (int i = 0; i < rt_nsteps; i++) {
        if (rt_steps[i].kind == K_DEPART) continue;
        if (rt_pts[rt_steps[i].idx].cum > prog + 3) { next = &rt_steps[i]; break; }
    }
    bool arrived = false;
    if (!off && remain < ARRIVE_M && (sim || now - rt_t0 > ARRIVE_MIN_US)) {
        arrived = true;
        arrive_t = now;
        off_since = 0;
        ESP_LOGI(TAG, "Chot den noi: con %.0f m, cach tuyen %.0f m%s", remain, best, sim ? ", tat SIM" : "");
        log_event("chot den noi: con %.0f m; cach tuyen %.0f m", remain, best);
        if (sim) sim_stop_keep_route();
    }
    taskENTER_CRITICAL(&nav_lock);
    uint8_t new_state = off ? NAV_OFF_ROUTE : arrived ? NAV_ARRIVED : NAV_NAVIGATING;
    static const char *STATE_NAME[] = { "idle", "dang dan duong", "lech tuyen", "da den noi" };
    if (new_state != nav_st.state) log_event("trang thai %s -> %s (cach tuyen %.0f m)", STATE_NAME[nav_st.state], STATE_NAME[new_state], best);
    nav_st.state = new_state;
    nav_st.next_step = next ? (uint16_t)(next - rt_steps) : 0xFFFF;
    nav_st.dist_next_m = next ? (uint32_t)fmaxf(rt_pts[next->idx].cum - prog, 0) : 0;
    nav_st.remain_m = (uint32_t)fmaxf(remain, 0);
    nav_st.lat_e7 = (int32_t)lround(g->lat * 1e7);
    nav_st.lon_e7 = (int32_t)lround(g->lon * 1e7);
    nav_st.speed_cms = (uint16_t)(g->speed_mps * 100);
    taskEXIT_CRITICAL(&nav_lock);
    lv_obj_remove_flag(turn_panel, LV_OBJ_FLAG_HIDDEN);
    // Lệch tuyến/đã đến: chữ báo dùng cả dòng 1 (quãng còn lại không cần); bình thường: khoảng cách 80 px + quãng còn lại
    bool special = off || arrived || !next;
    lv_obj_set_width(lbl_turn_dist, special ? 186 : 80);
    if (special) lv_obj_add_flag(lbl_remain, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(lbl_remain, LV_OBJ_FLAG_HIDDEN);
    char buf[48], rb[16];
    if (off) {
        if (shown_kind != K_UTURN) ESP_LOGI(TAG, "Chi dan: lech tuyen %.0f m", best);
        lv_obj_set_style_bg_color(turn_panel, lv_color_hex(theme_night ? 0x7A1A1A : 0xC62828), 0);
        lv_obj_set_style_text_font(lbl_turn_dist, &font_vn_16, 0);
        lv_label_set_text(lbl_turn_dist, "Lệch tuyến");
        fmt_dist(buf, sizeof(buf), best);
        lv_label_set_text_fmt(lbl_turn_name, "cách tuyến %s", buf);
        if (shown_kind != K_UTURN) { draw_turn_icon(K_UTURN); shown_kind = K_UTURN; }
    } else if (arrived || !next) {
        if (shown_kind != K_ARRIVE) ESP_LOGI(TAG, "Chi dan: da den noi");
        lv_obj_set_style_bg_color(turn_panel, lv_color_hex(theme_night ? 0x1B4D1F : 0x2E7D32), 0);
        lv_obj_set_style_text_font(lbl_turn_dist, &font_vn_16, 0);
        lv_label_set_text(lbl_turn_dist, "Đã đến nơi");
        lv_label_set_text(lbl_turn_name, rt_nsteps ? rt_steps[rt_nsteps - 1].name : "");
        if (shown_kind != K_ARRIVE) { draw_turn_icon(K_ARRIVE); shown_kind = K_ARRIVE; }
    } else {
        float d = rt_pts[next->idx].cum - prog;
        lv_obj_set_style_bg_color(turn_panel, lv_color_hex(theme_night ? 0x12284F : 0x1E4FB0), 0);
        if (d < 20) {
            lv_obj_set_style_text_font(lbl_turn_dist, &font_vn_16, 0);
            lv_label_set_text(lbl_turn_dist, next->kind == K_ARRIVE ? "Tới nơi" : "Rẽ ngay");
        } else {
            lv_obj_set_style_text_font(lbl_turn_dist, &lv_font_montserrat_20, 0);
            fmt_dist(buf, sizeof(buf), d);
            lv_label_set_text(lbl_turn_dist, buf);
        }
        if (next->kind == K_ROUNDABOUT && next->exit) lv_label_set_text_fmt(lbl_turn_name, "Lối ra %d - %s", next->exit, next->name);
        else lv_label_set_text(lbl_turn_name, next->name[0] ? next->name : "");
        if (shown_kind != next->kind) { draw_turn_icon(next->kind); shown_kind = next->kind; }
        static const rstep_t *last_next;
        if (next != last_next) {
            last_next = next;
            log_event("buoc re tiep: kieu %d %s; con %.0f m", next->kind, next->name, d);
            ESP_LOGI(TAG, "Chi dan: kieu %d, %s, con %.0f m (tong con %.0f m)", next->kind, next->name, d, remain);
        }
    }
    // Quãng còn lại + thời gian (theo tốc độ hiện tại, tối thiểu 20 km/h)
    float v = g->speed_mps > 5.5f ? g->speed_mps : 5.5f;
    fmt_dist(rb, sizeof(rb), remain > 0 ? remain : 0);
    lv_label_set_text_fmt(lbl_remain, "%s - %d phút", rb, (int)(remain / v / 60.0f + 0.99f));
}

static void layout_tiles(void);
static void reload_all(void);

// ---------- Hành trình giả lập dọc tuyến (SIM) ----------
#define SIM_CRUISE   (35 / 3.6f)   // m/s trên đường thẳng
#define SIM_TURN     (15 / 3.6f)   // m/s khi rẽ gấp / vòng xuyến
#define SIM_ACCEL    1.5f          // m/s^2
#define SIM_WAIT     30            // số điểm (10 Hz) đứng yên lúc đầu và lúc đến nơi

static float bearing_deg(float la1, float lo1, float la2, float lo2)
{
    float dx = (lo2 - lo1) * cosf((la1 + la2) * 0.5f * (float)M_PI / 180.0f), dy = la2 - la1;
    float b = atan2f(dx, dy) * 180.0f / (float)M_PI;
    return b < 0 ? b + 360 : b;
}

// Đi dọc tuyến, mỗi 0.1 s ghi 1 điểm; out = NULL thì chỉ đếm số điểm
static int sim_walk(simpt_t *out, const float *vmax)
{
    int k = 0;
    for (int w = 0; w < SIM_WAIT; w++, k++) {
        if (out) out[k] = (simpt_t){ rt_pts[0].lat, rt_pts[0].lon, 0, bearing_deg(rt_pts[0].lat, rt_pts[0].lon, rt_pts[1].lat, rt_pts[1].lon) };
    }
    int i = 0;
    float s = 0, v = 0, head = 0;
    while (i < rt_n - 1) {
        float seg = rt_pts[i + 1].cum - rt_pts[i].cum;
        // tốc độ cho phép theo quãng tăng tốc từ đầu đoạn và quãng phanh tới cuối đoạn
        float target = fminf(SIM_CRUISE, fminf(sqrtf(vmax[i] * vmax[i] + 2 * SIM_ACCEL * s),
                                              sqrtf(vmax[i + 1] * vmax[i + 1] + 2 * SIM_ACCEL * fmaxf(seg - s, 0))));
        v = v < target ? fminf(target, v + SIM_ACCEL / 10) : target;
        v = fmaxf(v, 0.5f);
        s += v / 10;
        while (i < rt_n - 1 && s >= rt_pts[i + 1].cum - rt_pts[i].cum) {
            s -= rt_pts[i + 1].cum - rt_pts[i].cum;
            i++;
        }
        if (i >= rt_n - 1) break;
        const rpt_t *a = &rt_pts[i], *b = &rt_pts[i + 1];
        float f = b->cum > a->cum ? s / (b->cum - a->cum) : 0;
        head = bearing_deg(a->lat, a->lon, b->lat, b->lon);
        if (out) out[k] = (simpt_t){ a->lat + (b->lat - a->lat) * f, a->lon + (b->lon - a->lon) * f, v, head };
        k++;
    }
    for (int w = 0; w < SIM_WAIT; w++, k++) {
        if (out) out[k] = (simpt_t){ rt_pts[rt_n - 1].lat, rt_pts[rt_n - 1].lon, 0, head };
    }
    return k;
}

// Tạo hành trình giả lập từ tuyến hiện tại (gọi trong task LVGL hoặc khi đã khoá LVGL)
static void sim_build_from_route(void)
{
    heap_caps_free(sim_trk);
    sim_trk = NULL;
    sim_trk_n = 0;
    if (rt_n < 2) return;
    // Tốc độ tối đa tại mỗi điểm theo độ đổi hướng trong khoảng ±15 m (bắt được cả cua tròn, vòng xuyến)
    float *vmax = heap_caps_malloc(rt_n * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!vmax) return;
    for (int i = 0; i < rt_n; i++) {
        if (i == 0 || i == rt_n - 1) { vmax[i] = 0; continue; }
        int a = i, b = i;
        while (a > 0 && rt_pts[i].cum - rt_pts[a].cum < 15) a--;
        while (b < rt_n - 1 && rt_pts[b].cum - rt_pts[i].cum < 15) b++;
        float turn = fabsf(fmodf(bearing_deg(rt_pts[i].lat, rt_pts[i].lon, rt_pts[b].lat, rt_pts[b].lon) -
                                 bearing_deg(rt_pts[a].lat, rt_pts[a].lon, rt_pts[i].lat, rt_pts[i].lon) + 540.0f, 360.0f) - 180.0f);
        vmax[i] = turn < 20 ? SIM_CRUISE : fmaxf(SIM_TURN, SIM_CRUISE * (1 - (turn - 20) / 70));
    }
    for (int i = 1; i < rt_n; i++) {          // giới hạn gia tốc 2 chiều
        vmax[i] = fminf(vmax[i], sqrtf(vmax[i - 1] * vmax[i - 1] + 2 * SIM_ACCEL * (rt_pts[i].cum - rt_pts[i - 1].cum)));
    }
    for (int i = rt_n - 2; i >= 0; i--) {
        vmax[i] = fminf(vmax[i], sqrtf(vmax[i + 1] * vmax[i + 1] + 2 * SIM_ACCEL * (rt_pts[i + 1].cum - rt_pts[i].cum)));
    }
    int n = sim_walk(NULL, vmax);
    sim_trk = heap_caps_malloc(n * sizeof(simpt_t), MALLOC_CAP_SPIRAM);
    if (sim_trk) sim_trk_n = sim_walk(sim_trk, vmax);
    heap_caps_free(vmax);
    sim_t0 = esp_timer_get_time();
    sim_restart = true;
    guidance_reset();
    ESP_LOGI(TAG, "SIM theo tuyen: %d diem (%.1f phut)", sim_trk_n, sim_trk_n / 600.0f);
}

// Lệnh thử: SIM rời tuyến, đi thẳng theo hướng hiện tại ~400 m rồi đứng chờ (để app tự tính lại đường).
// Khi app gửi tuyến mới, SIM chạy theo tuyến mới (nav_route_received gọi sim_build_from_route).
static void sim_go_off_route(void)
{
    if (!sim || !sim_trk || sim_trk_n == 0) return;
    simpt_t cur = sim_trk[((esp_timer_get_time() - sim_t0) / 100000) % sim_trk_n];
    const float v = 30 / 3.6f, len = 400;
    int moving = (int)(len / v * 10), n = moving + 10 * 60;      // đi thẳng rồi đứng chờ 60 s
    simpt_t *trk = heap_caps_malloc(n * sizeof(simpt_t), MALLOC_CAP_SPIRAM);
    if (!trk) return;
    float h = cur.heading * (float)M_PI / 180.0f, m_lon = 111320.0f * cosf(cur.lat * (float)M_PI / 180.0f);
    for (int k = 0; k < n; k++) {
        float d = (k < moving ? k : moving) * v / 10;
        trk[k] = (simpt_t){ cur.lat + d * cosf(h) / 111320.0f, cur.lon + d * sinf(h) / m_lon, k < moving ? v : 0, cur.heading };
    }
    heap_caps_free(sim_trk);
    sim_trk = trk;
    sim_trk_n = n;
    sim_t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "SIM roi tuyen: di thang %.0f m huong %.0f do", len, cur.heading);
}

// ---------- Giao tiếp với BLE (gọi từ task BLE, phải khoá LVGL khi đụng tới bản đồ/tuyến) ----------
static void route_ui_refresh(void)
{
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, 4, route_on ? 54 : 4);
    if (!route_on) lv_obj_add_flag(turn_panel, LV_OBJ_FLAG_HIDDEN);
    reload_all();                  // ghép lại ảnh bản đồ: vẽ tuyến mới / xoá tuyến cũ
}

// Đến nơi khi đang SIM: dừng SIM nhưng giữ tuyến (kể cả tuyến mẫu) cho tới hết lúc báo đến nơi
static void sim_stop_keep_route(void)
{
    sim = false;
    heap_caps_free(sim_trk);
    sim_trk = NULL;
    sim_trk_n = 0;
}

// Bật/tắt SIM (task LVGL hoặc đã khoá LVGL). Bật: chạy dọc tuyến BLE nếu có, không thì nạp tuyến mẫu.
// Tắt: gỡ tuyến mẫu, giữ tuyến BLE để dẫn đường bằng GPS thật.
static void sim_set(bool on, const char *source)
{
    ESP_LOGI(TAG, "Yeu cau SIM %s tu %s", on ? "bat" : "tat", source);
    log_event("SIM %s tu %s", on ? "bat" : "tat", source);
    guidance_reset();
    if (on) {
        if (rt_n < 2) route_load_demo();
        if (rt_n < 2) return;
        route_on = true;
        rt_seg = 0;
        sim = true;
        follow = true;
        lv_obj_add_flag(btn_follow, LV_OBJ_FLAG_HIDDEN);
        sim_build_from_route();
    } else {
        sim = false;
        heap_caps_free(sim_trk);
        sim_trk = NULL;
        sim_trk_n = 0;
        if (rt_is_demo) route_unload();
        rt_seg = 0;
    }
    ESP_LOGI(TAG, "SIM %s (%s)", sim ? "bat" : "tat", rt_n < 2 ? "khong co tuyen" : rt_is_demo ? "tuyen mau" : "tuyen BLE");
    route_ui_refresh();
}

bool nav_route_received(const uint8_t *data, size_t len, uint32_t crc)
{
    rpt_t *pts;
    rstep_t *steps;
    int n, ns;
    if (!route_parse(data, len, &pts, &n, &steps, &ns)) {
        ESP_LOGW(TAG, "Tuyen BLE sai dinh dang (%u byte)", (unsigned)len);
        return false;
    }
    lvgl_port_lock(0);
    rpt_t *old_pts = rt_pts;
    rstep_t *old_steps = rt_steps;
    rt_pts = pts;
    rt_n = n;
    rt_steps = steps;
    rt_nsteps = ns;
    rt_seg = 0;
    rt_crc = crc;
    rt_is_demo = false;
    route_on = true;
    guidance_reset();
    if (sim) sim_build_from_route();       // đang SIM (kể cả trên tuyến mẫu): chạy lại từ đầu tuyến mới
    route_ui_refresh();
    lvgl_port_unlock();
    heap_caps_free(old_pts);
    heap_caps_free(old_steps);
    ESP_LOGI(TAG, "Tuyen moi qua BLE: %d diem, %d buoc, %.0f m", n, ns, pts[n - 1].cum);
    log_event("nhan tuyen BLE: %u byte; CRC %08lx; %d diem; %d buoc; %.0f m", (unsigned)len, (unsigned long)crc, n, ns, pts[n - 1].cum);
    return true;
}

void nav_control(uint8_t cmd)
{
    log_event("lenh BLE %d", cmd);
    lvgl_port_lock(0);
    switch (cmd) {
    case CMD_START: route_on = rt_n >= 2; rt_seg = 0; guidance_reset(); break;
    case CMD_STOP:  route_on = false; guidance_reset(); break;
    case CMD_CLEAR:
        if (sim) sim_set(false, "BLE lenh 03");
        route_unload();
        break;
    case CMD_SIM_ON:  sim_set(true, "BLE lenh 04"); break;
    case CMD_SIM_OFF: sim_set(false, "BLE lenh 05"); break;
    case CMD_SIM_OFF_ROUTE: sim_go_off_route(); break;
    default: break;
    }
    route_ui_refresh();
    lvgl_port_unlock();
}

void nav_get_status(nav_status_t *st)
{
    taskENTER_CRITICAL(&nav_lock);
    *st = nav_st;
    taskEXIT_CRITICAL(&nav_lock);
    st->version = 1;
    st->flags = (gps.fix_ok && gps.fix_type >= 2 ? NAV_FLAG_GPS_FIX : 0) | (sim ? NAV_FLAG_SIM : 0) |
                (rt_n >= 2 ? NAV_FLAG_ROUTE_LOADED : 0) | (route_on ? NAV_FLAG_ROUTE_ACTIVE : 0);
    st->route_crc = rt_crc;
    if (theme_night) st->flags |= NAV_FLAG_NIGHT;
    st->reserved = 100;
}

static void layout_tiles(void)
{
    // Góc trên trái màn hình (khi chưa xoay) theo toạ độ thế giới; ô lưới tính từ hình vuông COVER_R quanh xe
    double left = cx - MARK_X, top = cy - MARK_Y;
    int tx0 = (int)floor((cx - COVER_R) / TILE), ty0 = (int)floor((cy - COVER_R) / TILE);
    int max_t = 1 << zoom;
    int lz = (use_lz4 ? 1 : 0) | (theme_night ? 2 : 0);

    // Ô z14 ở giữa: task nạp tile đổi bộ tên đường khi ô này đổi
    double z14 = pow(2, zoom - 14) * TILE;
    int wx = (int)floor(cx / z14), wy = (int)floor(cy / z14);
    if (wx != st_want_x || wy != st_want_y) {
        st_want_x = wx;
        st_want_y = wy;
        if (loader_handle) xTaskNotifyGive(loader_handle);
    }

    comp_tx0 = tx0;
    comp_ty0 = ty0;
    // Ảnh ghép đặt sao cho tile (tx0, ty0) ở đúng chỗ, xoay quanh vị trí xe
    int px = (int)lround(tx0 * TILE - left), py = (int)lround(ty0 * TILE - top);
    lv_obj_set_pos(map_img, px, py);
    lv_image_set_pivot(map_img, MARK_X - px, MARK_Y - py);
    lv_image_set_rotation(map_img, map_rot);

    bool changed = false;
    xSemaphoreTake(cache_mtx, portMAX_DELAY);
    req_n = 0;
    vis_n = 0;
    for (int j = 0; j < GRID_H; j++) {
        for (int i = 0; i < GRID_W; i++) {
            int k = j * GRID_W + i;
            int key[4] = { lz, zoom, tx0 + i, ty0 + j };
            uint16_t *dst = comp_buf + j * TILE * COMP_W + i * TILE;
            bool inside = key[2] >= 0 && key[3] >= 0 && key[2] < max_t && key[3] < max_t;
            tile_t *t = inside ? cache_find(key) : NULL;
            if (inside) memcpy(vis[vis_n++], key, sizeof(key));

            if (t && t->state == T_READY) {
                t->last_use = ++use_tick;
                if (!key_eq(tile_key[k], key)) {        // ghép tile vào ảnh lớn
                    const uint16_t *src = (const uint16_t *)t->buf;
                    for (int row = 0; row < TILE; row++) memcpy(dst + row * COMP_W, src + row * TILE, TILE * 2);
                    memcpy(tile_key[k], key, sizeof(key));
                    changed = true;
                }
                continue;
            }
            // Chưa có (hoặc ngoài vùng/không có file): tô nền xám 1 lần, xin loader nạp
            if (tile_key[k][0] != -1) {
                for (int row = 0; row < TILE; row++) {
                    for (int c = 0; c < TILE; c++) dst[row * COMP_W + c] = theme_night ? GREY_NIGHT_565 : GREY_565;
                }
                tile_key[k][0] = -1;
                changed = true;
            }
            if (inside && !t) memcpy(req[req_n++], key, sizeof(key));
        }
    }
    // Nạp trước 1 vòng tile xung quanh
    for (int y = ty0 - 1; y <= ty0 + GRID_H; y++) {
        for (int x = tx0 - 1; x <= tx0 + GRID_W; x++) {
            if (x >= tx0 && x < tx0 + GRID_W && y >= ty0 && y < ty0 + GRID_H) continue;
            if (x < 0 || y < 0 || x >= max_t || y >= max_t) continue;
            int key[4] = { lz, zoom, x, y };
            if (!cache_find(key) && req_n < MAX_REQ) memcpy(req[req_n++], key, sizeof(key));
        }
    }
    int n = req_n;
    xSemaphoreGive(cache_mtx);
    if (n > 0) xTaskNotifyGive(loader_handle);
    if (changed) {
        draw_route();                  // tuyến vẽ đè lên tile vừa ghép (vẽ lại cả tuyến, phần cũ trùng màu)
        lv_obj_invalidate(map_img);    // nội dung ảnh ghép đổi (cùng con trỏ dữ liệu)
        tiles_changed = true;
    }
    update_labels();
}

// Loader báo có tile mới: sắp xếp lại các ô (chạy trong task LVGL)
static void on_tiles_timer(lv_timer_t *t)
{
    if (!tiles_ready) return;
    tiles_ready = false;
    if (sd_ok != sd_ok_shown) update_status();
    layout_tiles();
}

static void reload_all(void)
{
    for (int k = 0; k < NSLOT; k++) tile_key[k][0] = -2;
    layout_tiles();
    update_status();
}

// ---------- Sự kiện ----------
static void on_drag(lv_event_t *e)
{
    lv_point_t v;
    lv_indev_get_vect(lv_indev_active(), &v);
    if (v.x == 0 && v.y == 0) return;
    if (follow) {
        follow = false;        // kéo tay: tạm dừng chạy theo GPS
        lv_obj_remove_flag(btn_follow, LV_OBJ_FLAG_HIDDEN);
    }
    // Bản đồ đang xoay map_rot: đổi vector kéo trên màn hình về hệ toạ độ bản đồ
    float a = -map_rot * (float)M_PI / 1800.0f;
    cx -= v.x * cosf(a) - v.y * sinf(a);
    cy -= v.x * sinf(a) + v.y * cosf(a);
    layout_tiles();
}

static void set_zoom(int z)
{
    if (z < ZOOM_MIN || z > ZOOM_MAX || z == zoom) return;
    double f = pow(2, z - zoom);
    cx *= f; cy *= f;
    zoom = z;
    reload_all();
}

// Zoom theo tốc độ (GPS, hoặc SIM khi đang SIM): chạy chậm thì gần hơn, trong 3 mức AUTOZOOM_Z.
// Từ gần ra xa: vượt ngưỡng thì đổi; từ xa vào gần: thấp hơn ngưỡng AUTOZOOM_HYST_KMH mới đổi.
#define AUTOZOOM_HYST_KMH   3.0f
#define AUTOZOOM_HOLD_US    5000000   // mức mới phải giữ chừng này mới đổi
#define AUTOZOOM_MANUAL_US  30000000  // bấm +/- thì thôi tự đổi chừng này
static const int AUTOZOOM_Z[3] = { 15, 16, 17 };          // xa, vừa, gần (z14 chỉ tới được bằng nút -)
static const float AUTOZOOM_BOUND_KMH[2] = { 45, 20 };    // ranh giới xa/vừa, vừa/gần
static int64_t zoom_manual_t;

static int autozoom_target(float kmh)
{
    int t = -1;
    for (int i = 0; i < 3; i++) if (AUTOZOOM_Z[i] == zoom) t = i;
    if (t < 0) return AUTOZOOM_Z[kmh > AUTOZOOM_BOUND_KMH[0] ? 0 : kmh > AUTOZOOM_BOUND_KMH[1] ? 1 : 2];   // đang ở mức chỉnh tay ngoài 3 mức
    while (t < 2 && kmh < AUTOZOOM_BOUND_KMH[t] - AUTOZOOM_HYST_KMH) t++;
    while (t > 0 && kmh > AUTOZOOM_BOUND_KMH[t - 1]) t--;
    return AUTOZOOM_Z[t];
}

// Tốc độ GPS có đáng tin để đổi zoom không. GPS chỉ ước tính (DR) hay sai số lớn thì giữ nguyên mức zoom.
// Log ngày 05/11: đứng yên ở nhà mà fix 4 (GNSS+DR) báo 77-90 km/h; các chuyến chạy thật đều là fix 3.
#define AUTOZOOM_MAX_HACC_M   30.0f
#define AUTOZOOM_MAX_SACC_MPS 2.0f
static bool zoom_speed_trusted(const gps_data_t *g)
{
    if (sim) return true;
    if (!g->fix_ok || (g->fix_type != 2 && g->fix_type != 3)) return false;   // 1 = chỉ DR, 4 = GNSS+DR
    if (g->h_acc_m > AUTOZOOM_MAX_HACC_M) return false;
    if (g->s_acc_mps > AUTOZOOM_MAX_SACC_MPS) return false;
    return true;
}

static void auto_zoom(float kmh, bool trusted, int64_t now)
{
    static int pending = -1;
    static int64_t pending_t;
    static bool was_trusted = true;
    if (trusted != was_trusted) {
        was_trusted = trusted;
        ESP_LOGI(TAG, "Zoom tu dong %s", trusted ? "chay lai" : "tam giu: toc do GPS khong tin duoc");
        log_event("zoom tu dong %s", trusted ? "chay lai" : "tam giu; GPS uoc tinh hoac sai so lon");
    }
    if (!trusted) { pending = -1; return; }
    if (zoom_manual_t && now - zoom_manual_t < AUTOZOOM_MANUAL_US) { pending = -1; return; }
    int want = autozoom_target(kmh);
    if (want == zoom) { pending = -1; return; }
    if (want != pending) { pending = want; pending_t = now; return; }
    if (now - pending_t < AUTOZOOM_HOLD_US) return;
    ESP_LOGI(TAG, "Zoom tu dong: %d -> %d (%.0f km/h)", zoom, want, kmh);
    log_event("zoom tu dong %d -> %d (%.0f km/h)", zoom, want, kmh);
    set_zoom(want);
    pending = -1;
}

static void on_zoom_in(lv_event_t *e)  { zoom_manual_t = esp_timer_get_time(); set_zoom(zoom + 1); }
static void on_zoom_out(lv_event_t *e) { zoom_manual_t = esp_timer_get_time(); set_zoom(zoom - 1); }

static void on_follow(lv_event_t *e)
{
    follow = true;
    lv_obj_add_flag(btn_follow, LV_OBJ_FLAG_HIDDEN);
}

static void on_toggle_mode(lv_event_t *e)
{
    heading_up = !heading_up;
    lv_label_set_text(lbl_mode, heading_up ? "H" : "N");
}


// Thanh tên đường đang đi: font 16 px, không vừa thì 14 px, vẫn không vừa thì chữ chạy vòng
#define STREET_W  (LCD_W - 8)
#define STREET_H  27             // = line_height font 16 px (25, gồm dấu tiếng Việt và chân chữ) + đệm trên/dưới
static void set_street_text(const char *st)
{
    lv_point_t sz;
    const lv_font_t *f = &font_vn_16;
    lv_text_get_size(&sz, st, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > STREET_W) {
        f = &font_vn_14;
        lv_text_get_size(&sz, st, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
    lv_obj_set_style_text_font(lbl_street, f, 0);
    lv_label_set_long_mode(lbl_street, sz.x > STREET_W ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP);
    lv_label_set_text_static(lbl_street, st);
}

// Làm mượt hướng: đi theo đường ngắn nhất tới hướng đích
static void smooth_heading(float target)
{
    float d = fmodf(target - disp_heading + 540.0f, 360.0f) - 180.0f;
    disp_heading = fmodf(disp_heading + d * 0.25f + 360.0f, 360.0f);
}

// Đặt góc xoay bản đồ và mũi tên theo chế độ; trả về true nếu bản đồ đổi góc
static bool apply_rotation(void)
{
    int rot = heading_up ? ((3600 - (int)lroundf(disp_heading * 10)) % 3600) : 0;
    int arrow_rot = heading_up ? 0 : (int)lroundf(disp_heading * 10);
    if (lv_obj_get_style_transform_rotation(arrow, 0) != arrow_rot) {
        lv_obj_set_style_transform_rotation(arrow, arrow_rot, 0);
    }
    if (map_rot >= 0) {
        int d = abs(rot - map_rot);
        if (d > 1800) d = 3600 - d;
        if (d < ROT_STEP) return false;         // đổi ít: giữ góc cũ, đỡ vẽ lại toàn màn hình
    }
    map_rot = rot;
    return true;
}

// Chạy theo GPS (mỗi 33 ms): nội suy vị trí theo tốc độ/hướng tới khi có HNR-PVT mới (tối đa 0.2 s)
static void on_follow_timer(lv_timer_t *t)
{
    static char last_status[48];
    static int64_t last_status_us;
    taskENTER_CRITICAL(&gps_lock);
    gps_data_t g = gps;
    taskEXIT_CRITICAL(&gps_lock);

    int64_t now = esp_timer_get_time();
    if (sim) sim_fill(&g, now);              // SIM: bỏ qua GPS thật
    if (now - last_status_us > 500000) {     // nhãn trạng thái + tốc độ 2 lần/giây
        last_status_us = now;
        update_status();
        bool fresh = g.have_pos && now - g.t_us < 2000000;
        if (fresh && follow) auto_zoom(g.speed_mps * 3.6f, zoom_speed_trusted(&g), now);
        char sp[16] = "--";
        if (fresh) snprintf(sp, sizeof(sp), "%d", (int)(g.speed_mps * 3.6f + 0.5f));
        if (strcmp(sp, last_status) != 0) {
            strcpy(last_status, sp);
            lv_label_set_text(lbl_speed, sp);
        }
        update_guidance(&g, now);
        static const char *last_street;
        {
            nav_status_t ns;
            taskENTER_CRITICAL(&nav_lock);
            ns = nav_st;
            taskEXIT_CRITICAL(&nav_lock);
            char line[LOG_LINE_MAX];
            snprintf(line, sizeof(line),
                     "P,%lld,%04d-%02d-%02dT%02d:%02d:%02dZ,%d,%d,%.1f,%.7f,%.7f,%.1f,%.0f,%d,%d,%d,%08lx,%d,%d,%.0f,%d,%lu,%lu,%d,%u,%u,%d,%d,%.1f,%s,\n",
                     now / 1000, g.year, g.month, g.day, g.hour, g.min, g.sec, sim ? 3 : g.fix_type, g.num_sv, g.h_acc_m,
                     g.have_pos ? g.lat : 0, g.have_pos ? g.lon : 0, g.speed_mps * 3.6f, g.heading, sim, ble_nav_connected(),
                     route_on, (unsigned long)rt_crc, route_on ? ns.state : 0, rt_seg, nav_route_dist,
                     ns.next_step == 0xFFFF ? -1 : ns.next_step, (unsigned long)ns.dist_next_m, (unsigned long)ns.remain_m,
                     fps_now, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), g.sv_view, g.cno_max, g.cno_mean,
                     last_street ? last_street : "");
            log_put(line);
        }
        const char *st = fresh ? current_street(g.lat, g.lon, g.heading, g.speed_mps > HEAD_MIN_MPS) : NULL;
        if (st != last_street) {
            last_street = st;
            ESP_LOGI(TAG, "Duong dang di: %s", st ? st : "-");
            if (st) {
                set_street_text(st);
                lv_obj_remove_flag(lbl_street, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(lbl_street, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    // Hướng: chỉ cập nhật khi đủ nhanh, chậm thì giữ hướng cũ
    static float target_heading;
    if (g.have_pos && g.speed_mps > HEAD_MIN_MPS) target_heading = g.heading;
    smooth_heading(target_heading);
    bool rotated = apply_rotation();

    if (!follow || !g.have_pos) {
        if (rotated) layout_tiles();
        return;
    }

    double lat = g.lat, lon = g.lon;
    float dt = (now - g.t_us) / 1e6f;
    if (g.speed_mps > 1.5f && dt > 0 && dt < gps_interp_s) {
        float d = g.speed_mps * dt;
        float h = g.heading * (float)M_PI / 180.0f;
        lat += d * cosf(h) / 111320.0;
        lon += d * sinf(h) / (111320.0 * cos(lat * M_PI / 180.0));
    }
    double x, y;
    latlon_to_px(lat, lon, zoom, &x, &y);
    if (!rotated && fabs(x - cx) < 0.5 && fabs(y - cy) < 0.5) return;   // không đổi thì không vẽ lại
    cx = x;
    cy = y;
    layout_tiles();
}

static void on_refr_start(lv_event_t *e) { refr_start = esp_timer_get_time(); }

static void on_refr_ready(lv_event_t *e)
{
    static int64_t stat_start, sec_start;
    static int stat_frames, stat_sum_ms, stat_max_ms, stat_min_ms, sec_frames, sec_sum_ms;
    int64_t now = esp_timer_get_time();
    int ms = (int)((now - refr_start) / 1000);
    if (stat_frames++ == 0) { stat_start = refr_start; stat_min_ms = ms; }
    stat_sum_ms += ms;
    if (ms > stat_max_ms) stat_max_ms = ms;
    if (ms < stat_min_ms) stat_min_ms = ms;
    if (now - stat_start > 5000000) {
        ESP_LOGI(TAG, "5s %s%s: %d frames (%.1f fps), render avg %d ms, min %d, max %d, hnr %lu, fix %d sv %d/%d cno %d/%.0f",
                 heading_up ? "HEADING-UP" : "NORTH-UP", sim ? " SIM" : "", stat_frames,
                 stat_frames * 1e6f / (now - stat_start), stat_sum_ms / stat_frames, stat_min_ms, stat_max_ms,
                 (unsigned long)hnr_count, gps.fix_type, gps.num_sv, gps.sv_view, gps.cno_max, gps.cno_mean);
        stat_frames = stat_sum_ms = stat_max_ms = 0;
    }
    if (sec_frames++ == 0) sec_start = refr_start;
    sec_sum_ms += ms;
    if (now - sec_start > 1000000) {
        fps_now = (int)(sec_frames * 1e6f / (now - sec_start) + 0.5f);
        frame_avg_ms = sec_sum_ms / sec_frames;
        sec_frames = sec_sum_ms = 0;
    }

    // Chỉ cập nhật nhãn sau khung hình có nạp tile (tránh vòng lặp vẽ lại do update_status)
    if (!tiles_changed) return;
    last_frame_ms = ms;
    tiles_changed = false;
}

// ---------- Cảm ứng XPT2046 ----------
static int xpt_read(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 24,
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data = { cmd, 0, 0, 0 },
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(touch, &t));
    return ((t.rx_data[1] << 8) | t.rx_data[2]) >> 3;
}

static int map_axis(int v, int raw_min, int raw_max, int size)
{
    int p = (v - raw_min) * size / (raw_max - raw_min);
    return p < 0 ? 0 : (p > size - 1 ? size - 1 : p);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->state = LV_INDEV_STATE_RELEASED;
    if (gpio_get_level(PIN_T_IRQ) != 0) return;
    if (xpt_read(0xB0) <= 100) return;
    int sx = 0, sy = 0;
    for (int i = 0; i < 4; i++) {
        sx += xpt_read(0xD0);
        sy += xpt_read(0x90);
    }
    data->point.x = map_axis(sy / 4, TOUCH_X_RAW_MIN, TOUCH_X_RAW_MAX, LCD_W);
    data->point.y = map_axis(sx / 4, TOUCH_Y_RAW_MIN, TOUCH_Y_RAW_MAX, LCD_H);
    data->state = LV_INDEV_STATE_PRESSED;
}

static void touch_init(void)
{
    spi_device_interface_config_t cfg = {
        .clock_speed_hz = TOUCH_CLK_HZ,
        .mode = 0,
        .spics_io_num = PIN_T_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &cfg, &touch));
    gpio_config_t irq = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << PIN_T_IRQ,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&irq));
    xpt_read(0xD0);   // lần đọc đầu bật chân T_IRQ

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
}

// ---------- Thẻ SD ----------
static sdmmc_card_t *sd_card;

static void sd_bus_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SD_SCK,
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SD_HOST, &bus, SDSPI_DEFAULT_DMA));
}

static bool sd_try_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SD_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount SD that bai: %s", esp_err_to_name(ret));
        sd_card = NULL;
        return false;
    }
    return true;
}

static void sd_unmount(void)
{
    if (!sd_card) return;
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, sd_card);
    sd_card = NULL;
}

// ---------- Màn hình ----------
static lv_display_t *display_init(void)
{
    gpio_config_t bl = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << PIN_BL };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(PIN_BL, 0);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 60 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_W * 60,
        .double_buffer = true,
        .hres = LCD_W,
        .vres = LCD_H,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = true, .mirror_x = true, .mirror_y = false },
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    gpio_set_level(PIN_BL, 1);
    return disp;
}

static lv_obj_t *theme_btns[6];
static int theme_btn_n;
static lv_obj_t *theme_box, *theme_unit, *theme_attr;

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, int y, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 40, 40);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, x, y);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    theme_btns[theme_btn_n++] = b;
    return b;
}

static void ui_create(lv_display_t *disp)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xE0E0E0), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_drag, LV_EVENT_PRESSING, NULL);

    comp_buf = heap_caps_malloc(COMP_W * COMP_H * 2, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(comp_buf ? ESP_OK : ESP_ERR_NO_MEM);
    comp_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    comp_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    comp_dsc.header.w = COMP_W;
    comp_dsc.header.h = COMP_H;
    comp_dsc.header.stride = COMP_W * 2;
    comp_dsc.data = (const uint8_t *)comp_buf;
    comp_dsc.data_size = COMP_W * COMP_H * 2;
    map_img = lv_image_create(scr);
    lv_image_set_src(map_img, &comp_dsc);
    lv_image_set_antialias(map_img, MAP_ANTIALIAS);
    lv_obj_remove_flag(map_img, LV_OBJ_FLAG_CLICKABLE);     // để kéo được trên bản đồ
    for (int k = 0; k < NSLOT; k++) tile_key[k][0] = -2;

    // Nhãn tên đường (nằm trên bản đồ, dưới các nút): chữ đậm trên nền trắng mờ để dễ đọc
    for (int k = 0; k < MAX_LABELS; k++) {
        lv_obj_t *o = lv_label_create(scr);
        lv_obj_set_style_text_font(o, &font_vn_14, 0);
        lv_obj_set_style_text_color(o, lv_color_hex(0x202020), 0);
        lv_obj_set_style_bg_color(o, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_70, 0);
        lv_obj_set_style_radius(o, 3, 0);
        lv_obj_set_style_pad_hor(o, 4, 0);
        lv_obj_set_style_pad_ver(o, 1, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        lbl_pool[k] = o;
        lbl_pool_name[k] = -1;
    }
    lv_timer_create(on_tiles_timer, 20, NULL);

    // Mũi tên xanh lá viền trắng = vị trí và hướng xe, vẽ 1 lần vào canvas nhỏ
    static uint8_t arrow_buf[LV_CANVAS_BUF_SIZE(28, 28, 32, LV_DRAW_BUF_STRIDE_ALIGN)];
    arrow = lv_canvas_create(scr);
    lv_canvas_set_buffer(arrow, arrow_buf, 28, 28, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(arrow, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(arrow, &layer);
    static const int ARROW[2][4][2] = {
        { {14, 0}, {27, 27}, {14, 20}, {1, 27} },    // viền trắng
        { {14, 4}, {24, 24}, {14, 18}, {4, 24} },    // thân xanh lá
    };
    for (int a = 0; a < 2; a++) {
        lv_draw_triangle_dsc_t tri;
        lv_draw_triangle_dsc_init(&tri);
        tri.bg_color = a ? lv_color_hex(0x1DB954) : lv_color_white();   // xanh lá, khác màu tuyến đường
        tri.bg_opa = LV_OPA_COVER;
        for (int h = 0; h < 2; h++) {     // mũi tên lõm = 2 tam giác ghép tại trục giữa
            int q = h ? 3 : 1;
            tri.p[0].x = ARROW[a][0][0]; tri.p[0].y = ARROW[a][0][1];
            tri.p[1].x = ARROW[a][q][0]; tri.p[1].y = ARROW[a][q][1];
            tri.p[2].x = ARROW[a][2][0]; tri.p[2].y = ARROW[a][2][1];
            lv_draw_triangle(&layer, &tri);
        }
    }
    lv_canvas_finish_layer(arrow, &layer);
    lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(arrow, MARK_X - 14, MARK_Y - 14);
    lv_obj_set_style_transform_pivot_x(arrow, 14, 0);
    lv_obj_set_style_transform_pivot_y(arrow, 14, 0);

    make_button(scr, "+", -6, 6, on_zoom_in);
    make_button(scr, "-", -6, 50, on_zoom_out);
    lv_obj_t *b = make_button(scr, "H", -6, 94, on_toggle_mode);      // H = heading-up, N = north-up
    lbl_mode = lv_obj_get_child(b, 0);
    btn_follow = make_button(scr, LV_SYMBOL_GPS, -60, 6, on_follow);
    lv_obj_add_flag(btn_follow, LV_OBJ_FLAG_HIDDEN);   // chỉ hiện khi đã kéo bản đồ đi chỗ khác

    // Tốc độ (km/h) chữ lớn ở góc dưới trái
    lv_obj_t *box = lv_obj_create(scr);
    theme_box = box;
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_60, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_ver(box, 2, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_align(box, LV_ALIGN_BOTTOM_LEFT, 4, -(STREET_H + 4));   // ngay trên dải tên đường, cách 4 px
    lv_obj_set_style_pad_hor(box, 6, 0);
    lbl_speed = lv_label_create(box);
    lv_obj_set_style_text_font(lbl_speed, &lv_font_montserrat_38, 0);
    lv_obj_set_style_text_color(lbl_speed, lv_color_white(), 0);
    lv_label_set_text(lbl_speed, "--");
    lv_obj_t *unit = lv_label_create(box);              // đơn vị nhỏ, mờ, góc trên phải con số
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(0xA0A0A0), 0);
    theme_unit = unit;
    lv_obj_set_style_pad_top(unit, 6, 0);
    lv_obj_set_style_pad_left(unit, 2, 0);
    lv_label_set_text(unit, "km/h");

    // Hộp trạng thái gọn: [GPS màu theo fix] số vệ tinh  [SIM]  [Bluetooth]  fps
    lbl_status = lv_obj_create(scr);
    lv_obj_set_size(lbl_status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(lbl_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lbl_status, LV_OPA_60, 0);
    lv_obj_set_style_border_width(lbl_status, 0, 0);
    lv_obj_set_style_radius(lbl_status, 4, 0);
    lv_obj_set_style_pad_hor(lbl_status, 5, 0);
    lv_obj_set_style_pad_ver(lbl_status, 2, 0);
    lv_obj_set_style_pad_column(lbl_status, 5, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_white(), 0);
    lv_obj_set_flex_flow(lbl_status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lbl_status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(lbl_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(lbl_status, LV_OBJ_FLAG_SCROLLABLE);
    st_gps = lv_label_create(lbl_status);
    lv_label_set_text(st_gps, LV_SYMBOL_GPS);
    st_sv = lv_label_create(lbl_status);
    lv_label_set_text(st_sv, "0");
    st_sim = lv_label_create(lbl_status);
    lv_label_set_text(st_sim, "SIM");
    lv_obj_set_style_text_color(st_sim, lv_color_white(), 0);       // nền đỏ đặc: chữ đỏ trên nền tối khó thấy ngoài trời
    lv_obj_set_style_bg_color(st_sim, lv_color_hex(0xD32020), 0);
    lv_obj_set_style_bg_opa(st_sim, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(st_sim, 3, 0);
    lv_obj_set_style_pad_hor(st_sim, 4, 0);
    lv_obj_add_flag(st_sim, LV_OBJ_FLAG_HIDDEN);
    st_ble = lv_label_create(lbl_status);
    lv_label_set_text(st_ble, LV_SYMBOL_BLUETOOTH);
    st_log = lv_label_create(lbl_status);
    lv_label_set_text(st_log, LV_SYMBOL_SD_CARD);
    lv_obj_set_style_text_color(st_log, lv_color_hex(0x40E060), 0);
    lv_obj_add_flag(st_log, LV_OBJ_FLAG_HIDDEN);
    st_fps = lv_label_create(lbl_status);
    lv_obj_set_style_text_font(st_fps, &lv_font_montserrat_12, 0);
    lv_label_set_text(st_fps, "");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, 4, route_on ? 54 : 4);   // có tuyến: nằm dưới ô chỉ dẫn

    // Ô chỉ dẫn rẽ 240x46: mũi tên | dòng 1: khoảng cách tới chỗ rẽ (trái) + quãng còn lại, thời gian (phải, cố định)
    //                          | dòng 2: tên đường sẽ rẽ vào
    turn_panel = lv_obj_create(scr);
    lv_obj_set_size(turn_panel, 240, 46);
    lv_obj_align(turn_panel, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_color(turn_panel, lv_color_hex(0x1E4FB0), 0);
    lv_obj_set_style_bg_opa(turn_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(turn_panel, 0, 0);
    lv_obj_set_style_radius(turn_panel, 6, 0);
    lv_obj_set_style_pad_all(turn_panel, 3, 0);
    lv_obj_remove_flag(turn_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(turn_panel, LV_OBJ_FLAG_SCROLLABLE);
    turn_icon = lv_canvas_create(turn_panel);
    lv_canvas_set_buffer(turn_icon, turn_icon_buf, 48, 48, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_style_transform_scale(turn_icon, 256 * 40 / 48, 0);      // vẽ 48x48, hiện 40x40
    lv_obj_set_style_transform_pivot_x(turn_icon, 0, 0);
    lv_obj_set_style_transform_pivot_y(turn_icon, 24, 0);
    lv_obj_align(turn_icon, LV_ALIGN_LEFT_MID, 0, 0);
    lbl_turn_dist = lv_label_create(turn_panel);
    lv_obj_set_style_text_color(lbl_turn_dist, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_turn_dist, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(lbl_turn_dist, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_turn_dist, 80);                        // "12.5 km" ~75 px
    lv_obj_align(lbl_turn_dist, LV_ALIGN_TOP_LEFT, 44, -3);
    lbl_turn_name = lv_label_create(turn_panel);
    lv_obj_set_style_text_color(lbl_turn_name, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_turn_name, &font_vn_14, 0);
    lv_label_set_long_mode(lbl_turn_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_turn_name, 186);
    lv_obj_align(lbl_turn_name, LV_ALIGN_BOTTOM_LEFT, 44, 3);
    lbl_remain = lv_label_create(turn_panel);
    lv_obj_set_style_text_color(lbl_remain, lv_color_hex(0xC8D8FF), 0);
    lv_obj_set_style_text_font(lbl_remain, &font_vn_12, 0);
    lv_obj_set_style_text_align(lbl_remain, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(lbl_remain, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_remain, 110);                          // "12.5 km - 125 phút" = 110 px, không chồng khoảng cách
    lv_obj_align(lbl_remain, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_label_set_text(lbl_remain, "");
    lv_obj_add_flag(turn_panel, LV_OBJ_FLAG_HIDDEN);

    // Thanh tên đường đang đi: dải ngang cả màn hình ở cạnh dưới
    lbl_street = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_street, &font_vn_16, 0);
    lv_obj_set_style_text_color(lbl_street, lv_color_white(), 0);
    lv_obj_set_style_bg_color(lbl_street, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lbl_street, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(lbl_street, 4, 0);
    lv_obj_set_style_pad_top(lbl_street, -1, 0);      // chữ dịch lên 2 px so với đệm đều 1 px, giữ nguyên chiều cao dải
    lv_obj_set_style_pad_bottom(lbl_street, 3, 0);
    lv_obj_set_style_text_align(lbl_street, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(lbl_street, LCD_W, STREET_H);        // cả chiều ngang màn hình để tên dài vẫn hiện đủ
    lv_label_set_long_mode(lbl_street, LV_LABEL_LONG_CLIP);
    lv_obj_align(lbl_street, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(lbl_street, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *attr = lv_label_create(scr);
    theme_attr = attr;
    lv_label_set_text(attr, "(c) OpenStreetMap");     // dạng rút gọn cho màn nhỏ
    lv_obj_set_style_text_font(attr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(attr, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_color(attr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(attr, LV_OPA_70, 0);
    lv_obj_align(attr, LV_ALIGN_BOTTOM_RIGHT, -2, -(STREET_H + 4));

    lv_display_add_event_cb(disp, on_refr_start, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, on_refr_ready, LV_EVENT_REFR_READY, NULL);
    lv_timer_create(on_follow_timer, 33, NULL);

    latlon_to_px(START_LAT, START_LON, zoom, &cx, &cy);
    reload_all();
}

// ---------- Giao diện ngày/đêm ----------
static void theme_apply(void)
{
    const bool n = theme_night;
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(n ? 0x101216 : 0xE0E0E0), 0);
    for (int i = 0; i < theme_btn_n; i++) {
        lv_obj_set_style_bg_color(theme_btns[i], n ? lv_color_hex(0x2A2F38) : lv_theme_get_color_primary(NULL), 0);
        lv_obj_set_style_text_color(theme_btns[i], n ? lv_color_hex(0xD0D4DC) : lv_color_white(), 0);
    }
    for (int k = 0; k < MAX_LABELS; k++) {       // tên đường trên bản đồ
        lv_obj_set_style_text_color(lbl_pool[k], lv_color_hex(n ? 0xE6E6E6 : 0x202020), 0);
        lv_obj_set_style_bg_color(lbl_pool[k], n ? lv_color_black() : lv_color_white(), 0);
    }
    lv_obj_set_style_text_color(theme_attr, lv_color_hex(n ? 0xB0B0B0 : 0x303030), 0);
    lv_obj_set_style_bg_color(theme_attr, n ? lv_color_black() : lv_color_white(), 0);
    lv_opa_t dark = n ? LV_OPA_80 : LV_OPA_60;   // các hộp nền đen
    lv_obj_set_style_bg_opa(theme_box, dark, 0);
    lv_obj_set_style_bg_opa(lbl_status, dark, 0);
    lv_obj_set_style_bg_opa(lbl_street, dark, 0);
    lv_obj_set_style_text_color(lbl_speed, lv_color_hex(n ? 0xD8D8D8 : 0xFFFFFF), 0);
    lv_obj_set_style_text_color(lbl_street, lv_color_hex(n ? 0xD8D8D8 : 0xFFFFFF), 0);
    lv_obj_set_style_text_color(theme_unit, lv_color_hex(n ? 0x808080 : 0xA0A0A0), 0);
    lv_obj_set_style_bg_color(turn_panel, lv_color_hex(n ? 0x12284F : 0x1E4FB0), 0);
}

static void theme_load(void)
{
    nvs_flash_init();              // ble_nav_init sẽ xử lý trường hợp cần xoá NVS
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("nav", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "night", &v);
        nvs_close(h);
    }
    theme_night = v != 0;
    ESP_LOGI(TAG, "Giao dien: %s", theme_night ? "dem" : "ngay");
}

// Đổi giao diện: xoá cache tile rồi ghép lại ảnh bản đồ; thư mục tile chọn trong load_tile theo bit đêm của khoá
void nav_set_theme(bool night)
{
    if (night == theme_night) return;
    lvgl_port_lock(0);
    theme_night = night;
    xSemaphoreTake(cache_mtx, portMAX_DELAY);
    for (int i = 0; i < CACHE_N; i++) {
        if (cache[i].state != T_LOADING) cache[i].state = T_FREE;
    }
    req_n = 0;
    xSemaphoreGive(cache_mtx);
    theme_apply();
    reload_all();
    lvgl_port_unlock();
    nvs_handle_t h;
    if (nvs_open("nav", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "night", night);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Giao dien: %s", night ? "dem" : "ngay");
    log_event("giao dien %s", night ? "dem" : "ngay");
}

void app_main(void)
{
    sd_bus_init();
    sd_ok = sd_try_mount();      // nếu lỗi, task loader sẽ thử mount lại
    theme_load();
    lv_display_t *disp = display_init();

    cache_init();
    streets_init();
    lvgl_port_lock(0);
    touch_init();
    ui_create(disp);
    lvgl_port_lock(0);
    theme_apply();
    lvgl_port_unlock();
    lvgl_port_unlock();
    log_init();
    gps_init();
    ble_nav_init();
    ESP_LOGI(TAG, "RAM trong: noi %u KB, PSRAM %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ESP_LOGI(TAG, "Started");
}
