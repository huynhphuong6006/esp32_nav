/*
 * BLE (NimBLE, peripheral): dịch vụ dẫn đường "NavESP". Giao thức chi tiết: docs/ble_protocol.md
 *   route   (write, write no rsp): nhận tuyến chia gói BEGIN / DATA / END, kiểm CRC32 rồi giao cho main.c
 *   status  (read, notify)       : nav_status_t, gửi khi thay đổi và mỗi 2 giây
 *   control (write)              : 1 byte lệnh CMD_*
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble_nav.h"

static const char *TAG = "ble_nav";

#define DEVICE_NAME      "NavESP"
#define ROUTE_MAX_BYTES  (256 * 1024)       // giới hạn kích thước tuyến nhận qua BLE
#define STATUS_PERIOD_MS 2000

// UUID 128-bit (little-endian), dạng chuỗi: 4e415600-ba5e-4c0d-8e2f-6a1b3c5d7e9X
#define NAV_UUID(last) BLE_UUID128_INIT(last, 0x7e, 0x5d, 0x3c, 0x1b, 0x6a, 0x2f, 0x8e, 0x0d, 0x4c, 0x5e, 0xba, 0x00, 0x56, 0x41, 0x4e)
static const ble_uuid128_t UUID_SVC     = NAV_UUID(0x90);
static const ble_uuid128_t UUID_ROUTE   = NAV_UUID(0x91);
static const ble_uuid128_t UUID_STATUS  = NAV_UUID(0x92);
static const ble_uuid128_t UUID_CONTROL = NAV_UUID(0x93);

enum { PKT_BEGIN = 1, PKT_DATA = 2, PKT_END = 3 };

static uint8_t own_addr_type;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int8_t last_rssi = 127;             // RSSI đọc mỗi 2 giây khi đang kết nối; 127 = chưa có
static int64_t connected_at;

// Tên các mã lý do ngắt hay gặp (HCI)
static const char *hci_reason_name(int hci)
{
    switch (hci) {
    case 0x08: return "het thoi gian cho (mat song)";
    case 0x13: return "dien thoai chu dong ngat";
    case 0x16: return "thiet bi chu dong ngat";
    case 0x22: return "het thoi gian cho giao thuc";
    case 0x3B: return "tham so ket noi khong chap nhan";
    case 0x3D: return "loi MIC";
    case 0x3E: return "khong lap duoc ket noi";
    default: return "khac";
    }
}

static void log_conn_params(const char *what, uint16_t handle)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(handle, &d) != 0) return;
    char m[96];
    snprintf(m, sizeof(m), "%s; chu ky %.1f ms; latency %d; timeout %d ms", what,
             d.conn_itvl * 1.25f, d.conn_latency, d.supervision_timeout * 10);
    ESP_LOGI(TAG, "%s", m);
    nav_log_event(m);
}
static uint16_t status_handle;
static bool status_subscribed;
static volatile bool status_dirty;
static volatile uint8_t upload_result = UPLOAD_NONE;

// Bộ đệm nhận tuyến (PSRAM)
static uint8_t *rx_buf;
static uint32_t rx_total, rx_len, rx_crc;

static void advertise(void);

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void set_upload(uint8_t r)
{
    upload_result = r;
    status_dirty = true;
}

// Một gói ghi vào characteristic route
static void route_packet(const uint8_t *p, int len)
{
    if (len < 1) return;
    switch (p[0]) {
    case PKT_BEGIN:
        if (len < 9) { set_upload(UPLOAD_ERR_FORMAT); return; }
        rx_total = rd32(p + 1);
        rx_crc = rd32(p + 5);
        rx_len = 0;
        heap_caps_free(rx_buf);
        rx_buf = NULL;
        if (rx_total == 0 || rx_total > ROUTE_MAX_BYTES ||
            !(rx_buf = heap_caps_malloc(rx_total, MALLOC_CAP_SPIRAM))) {
            set_upload(UPLOAD_ERR_SIZE);
            return;
        }
        set_upload(UPLOAD_RECEIVING);
        ESP_LOGI(TAG, "Bat dau nhan tuyen %lu byte", (unsigned long)rx_total);
        break;
    case PKT_DATA: {
        if (!rx_buf || len < 5) { set_upload(UPLOAD_ERR_SEQUENCE); return; }
        uint32_t off = rd32(p + 1), n = len - 5;
        if (off != rx_len || off + n > rx_total) {     // gói phải nối tiếp đúng thứ tự
            set_upload(UPLOAD_ERR_SEQUENCE);
            heap_caps_free(rx_buf);
            rx_buf = NULL;
            return;
        }
        memcpy(rx_buf + off, p + 5, n);
        rx_len += n;
        break;
    }
    case PKT_END: {
        if (!rx_buf || rx_len != rx_total) { set_upload(UPLOAD_ERR_SEQUENCE); return; }
        uint32_t crc = esp_rom_crc32_le(0, rx_buf, rx_len);
        if (crc != rx_crc) {
            ESP_LOGW(TAG, "Sai CRC: nhan %08lx, tinh %08lx", (unsigned long)rx_crc, (unsigned long)crc);
            set_upload(UPLOAD_ERR_CRC);
        } else if (!nav_route_received(rx_buf, rx_len, crc)) {
            set_upload(UPLOAD_ERR_FORMAT);
        } else {
            ESP_LOGI(TAG, "Nhan tuyen xong: %lu byte, CRC %08lx", (unsigned long)rx_len, (unsigned long)crc);
            set_upload(UPLOAD_OK);
        }
        heap_caps_free(rx_buf);
        rx_buf = NULL;
        break;
    }
    default:
        set_upload(UPLOAD_ERR_FORMAT);
    }
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid = ctxt->chr->uuid;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ble_uuid_cmp(uuid, &UUID_STATUS.u) == 0) {
        nav_status_t st;
        nav_get_status(&st);
        st.upload = upload_result;
        return os_mbuf_append(ctxt->om, &st, sizeof(st)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        static uint8_t buf[512];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (ble_uuid_cmp(uuid, &UUID_ROUTE.u) == 0) {
            route_packet(buf, len);
        } else if (ble_uuid_cmp(uuid, &UUID_CONTROL.u) == 0 && len >= 1) {
            ESP_LOGI(TAG, "Lenh %d", buf[0]);
            if (buf[0] == CMD_THEME) {
                if (len >= 2) nav_set_theme(buf[1] != 0);
            } else {
                nav_control(buf[0]);
            }
            status_dirty = true;
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &UUID_ROUTE.u, .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &UUID_STATUS.u, .access_cb = gatt_access, .val_handle = &status_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &UUID_CONTROL.u, .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_WRITE },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            conn_handle = ev->connect.conn_handle;
            connected_at = esp_timer_get_time();
            last_rssi = 127;
            log_conn_params("BLE ket noi", conn_handle);
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
    {
        int r = ev->disconnect.reason;
        int hci = r >= BLE_HS_ERR_HCI_BASE ? r - BLE_HS_ERR_HCI_BASE : -1;
        char m[128];
        snprintf(m, sizeof(m), "BLE mat ket noi; ly do 0x%02X %s; RSSI cuoi %d; sau %lld s", hci < 0 ? r : hci,
                 hci < 0 ? "loi host" : hci_reason_name(hci), last_rssi == 127 ? 0 : last_rssi,
                 (esp_timer_get_time() - connected_at) / 1000000);
        ESP_LOGI(TAG, "%s", m);
        nav_log_event(m);
    }
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        status_subscribed = false;
        heap_caps_free(rx_buf);            // bỏ tuyến đang nhận dở
        rx_buf = NULL;
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == status_handle) {
            status_subscribed = ev->subscribe.cur_notify;
            status_dirty = true;
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ev->conn_update.status == 0) log_conn_params("BLE doi tham so", ev->conn_update.conn_handle);
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", ev->mtu.value);
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)DEVICE_NAME;
    f.name_len = strlen(DEVICE_NAME);
    f.name_is_complete = 1;
    f.uuids128 = &UUID_SVC;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) { ESP_LOGE(TAG, "adv fields %d", rc); return; }
    struct ble_gap_adv_params p = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN };
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv start %d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    advertise();
    ESP_LOGI(TAG, "Dang quang ba \"%s\"", DEVICE_NAME);
}

static void on_reset(int reason) { ESP_LOGW(TAG, "NimBLE reset %d", reason); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// Gửi notify trạng thái khi có thay đổi (state/bước/kết quả gửi tuyến) hoặc mỗi 2 giây
static void status_task(void *arg)
{
    nav_status_t last = { 0 };
    TickType_t last_sent = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (conn_handle == BLE_HS_CONN_HANDLE_NONE) continue;
        static TickType_t last_rssi_read;
        if (xTaskGetTickCount() - last_rssi_read > pdMS_TO_TICKS(2000)) {
            last_rssi_read = xTaskGetTickCount();
            int8_t v;
            if (ble_gap_conn_rssi(conn_handle, &v) == 0) last_rssi = v;
        }
        if (!status_subscribed) continue;
        nav_status_t st;
        nav_get_status(&st);
        st.upload = upload_result;
        bool changed = status_dirty || st.state != last.state || st.next_step != last.next_step ||
                       st.upload != last.upload || st.flags != last.flags;
        if (!changed && xTaskGetTickCount() - last_sent < pdMS_TO_TICKS(STATUS_PERIOD_MS)) continue;
        status_dirty = false;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&st, sizeof(st));
        if (om && ble_gatts_notify_custom(conn_handle, status_handle, om) == 0) {
            last = st;
            last_sent = xTaskGetTickCount();
        }
    }
}

void ble_nav_status_changed(void) { status_dirty = true; }

bool ble_nav_connected(void) { return conn_handle != BLE_HS_CONN_HANDLE_NONE; }

void ble_nav_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    // Xoá độ sáng do bản firmware cũ đã lưu (tính năng đã bỏ, không còn code nào đọc)
    nvs_handle_t nh;
    if (nvs_open("nav", NVS_READWRITE, &nh) == ESP_OK) {
        if (nvs_erase_key(nh, "bl") == ESP_OK) {
            nvs_commit(nh);
            ESP_LOGI(TAG, "Da xoa do sang cu trong NVS");
        }
        nvs_close(nh);
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ble_svc_gap_device_name_set(DEVICE_NAME);
    nimble_port_freertos_init(host_task);
    xTaskCreate(status_task, "ble_status", 3072, NULL, 3, NULL);
    // Kiểm tra CRC32 khớp chuẩn zlib (app dùng zlib/CRC32 chuẩn)
    if (esp_rom_crc32_le(0, (const uint8_t *)"123456789", 9) != 0xCBF43926) ESP_LOGE(TAG, "CRC32 khong khop chuan!");
}
