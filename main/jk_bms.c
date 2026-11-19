#include "jk_bms.h"
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "config.h"
#include "settings.h"

static const char *TAG = "jk_bms";

/*
 * Кадр:  4E 57 | LEN(2) | TERMINAL(4) | CMD | SRC | TYPE | DATA... | RECORD(4) | 68 | CRC(4)
 * LEN = длина кадра без первых двух байт.
 * CRC = 16-битная сумма всех байт от 4E до 68 включительно, в двух последних байтах.
 */
#define JK_HDR_LEN   11
#define JK_TAIL_LEN  9      // RECORD(4) + 0x68 + CRC(4)
#define JK_RX_MAX    512

static uint8_t s_req[21] = {
    0x4E, 0x57, 0x00, 0x13,         // заголовок, LEN = 19
    0x00, 0x00, 0x00, 0x00,         // terminal id
    0x06,                           // команда: прочитать все данные
    0x03,                           // источник: ПК
    0x00,                           // тип: запрос
    0x00,                           // ID данных: 0 = все
    0x00, 0x00, 0x00, 0x00,         // record number
    0x68,                           // конец
    0x00, 0x00, 0x00, 0x00,         // CRC, считается в jk_bms_init()
};

static uint8_t s_rx[JK_RX_MAX];

// Диагностика: что пришло в текущем опросе и итоги по всем опросам
static uint16_t      s_cnt;
static uint8_t       s_cap[JK_DIAG_RAW];
static uint8_t       s_cap_len;
static jk_bms_diag_t s_diag;
static portMUX_TYPE  s_diag_mux = portMUX_INITIALIZER_UNLOCKED;

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t jk_sum(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return (uint16_t)s;
}

esp_err_t jk_bms_init(void)
{
    uint16_t crc = jk_sum(s_req, sizeof(s_req) - 4);   // 0x0129
    s_req[19] = crc >> 8;
    s_req[20] = crc & 0xFF;

    const uart_config_t cfg = {
        .baud_rate  = BMS_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(BMS_UART_NUM, 1024, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(BMS_UART_NUM, &cfg);
    if (err == ESP_OK) err = uart_set_pin(BMS_UART_NUM, BMS_UART_TX_PIN, BMS_UART_RX_PIN,
                                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    return err;
}

// Читает ровно n байт до дедлайна (мкс). Возвращает true, если успели.
static bool read_exact(uint8_t *dst, size_t n, int64_t deadline_us)
{
    size_t got = 0;
    while (got < n) {
        int64_t left_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (left_ms <= 0) return false;
        int r = uart_read_bytes(BMS_UART_NUM, dst + got, n - got, pdMS_TO_TICKS(left_ms) + 1);
        if (r < 0) return false;
        for (int k = 0; k < r && s_cap_len < JK_DIAG_RAW; k++) s_cap[s_cap_len++] = dst[got + k];
        s_cnt += r;
        got += r;
    }
    return true;
}

static esp_err_t poll_frame(jk_bms_data_t *out, jk_result_t *res)
{
    uart_flush_input(BMS_UART_NUM);
    uart_write_bytes(BMS_UART_NUM, s_req, sizeof(s_req));

    const int64_t deadline = esp_timer_get_time() + BMS_RESPONSE_TIMEOUT_MS * 1000LL;

    // Ищем начало кадра 4E 57
    uint8_t b;
    int state = 0;
    while (state < 2) {
        if (!read_exact(&b, 1, deadline)) {
            *res = s_cnt ? JK_RES_GARBAGE : JK_RES_NO_REPLY;
            return ESP_ERR_TIMEOUT;
        }
        if (b == 0x4E)                    state = 1;
        else if (state == 1 && b == 0x57) state = 2;
        else                              state = 0;
    }
    *res = JK_RES_BAD_FRAME;
    s_rx[0] = 0x4E;
    s_rx[1] = 0x57;
    if (!read_exact(s_rx + 2, 2, deadline)) return ESP_ERR_TIMEOUT;

    size_t total = (size_t)be16(s_rx + 2) + 2;
    if (total < JK_HDR_LEN + JK_TAIL_LEN || total > JK_RX_MAX) {
        ESP_LOGW(TAG, "странная длина кадра: %u", (unsigned)total);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!read_exact(s_rx + 4, total - 4, deadline)) {
        ESP_LOGW(TAG, "кадр оборвался");
        return ESP_ERR_TIMEOUT;
    }
#if BMS_DEBUG_DUMP
    ESP_LOG_BUFFER_HEX(TAG, s_rx, total);
#endif
    if (total == sizeof(s_req) && memcmp(s_rx, s_req, total) == 0) {
        ESP_LOGW(TAG, "получен собственный запрос — TX и RX замкнуты?");
        *res = JK_RES_ECHO;
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!jk_bms_parse(s_rx, total, out)) return ESP_ERR_INVALID_RESPONSE;
    *res = JK_RES_OK;
    return ESP_OK;
}

esp_err_t jk_bms_poll(jk_bms_data_t *out)
{
    s_cnt = 0;
    s_cap_len = 0;
    jk_result_t res = JK_RES_NO_REPLY;
    esp_err_t err = poll_frame(out, &res);

    portENTER_CRITICAL(&s_diag_mux);
    s_diag.polls++;
    switch (res) {
    case JK_RES_OK:        s_diag.ok++;        break;
    case JK_RES_NO_REPLY:  s_diag.no_reply++;  break;
    case JK_RES_GARBAGE:   s_diag.garbage++;   break;
    case JK_RES_BAD_FRAME: s_diag.bad_frame++; break;
    case JK_RES_ECHO:      s_diag.echo++;      break;
    default: break;
    }
    s_diag.last = res;
    s_diag.last_bytes = s_cnt;
    memcpy(s_diag.raw, s_cap, s_cap_len);
    s_diag.raw_len = s_cap_len;
    portEXIT_CRITICAL(&s_diag_mux);
    return err;
}

void jk_bms_get_diag(jk_bms_diag_t *out)
{
    portENTER_CRITICAL(&s_diag_mux);
    *out = s_diag;
    portEXIT_CRITICAL(&s_diag_mux);
}

// Длина данных поля после байта ID; -1 — неизвестное поле. 0x79 обрабатывается отдельно.
static int field_len(uint8_t id)
{
    switch (id) {
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: return 2;
    case 0x85: case 0x86:                                  return 1;
    case 0x87:                                             return 2;
    case 0x89:                                             return 4;
    case 0x8A: case 0x8B: case 0x8C:                       return 2;
    case 0x9D: case 0xA9: case 0xAB: case 0xAC:
    case 0xAE: case 0xAF: case 0xB1: case 0xB3: case 0xB8:
    case 0xBB: case 0xBC: case 0xBD: case 0xC0:            return 1;
    case 0xAA: case 0xB5: case 0xB6: case 0xB9:            return 4;
    case 0xB2:                                             return 10;
    case 0xB4:                                             return 8;
    case 0xB7:                                             return 15;
    case 0xBA:                                             return 24;
    case 0xAD: case 0xB0: case 0xBE: case 0xBF:            return 2;
    default:
        if (id >= 0x8E && id <= 0x9C) return 2;   // уставки защит
        if (id >= 0x9E && id <= 0xA8) return 2;   // температурные уставки
        return -1;
    }
}

// Температура: 0..100 = плюс, 101..140 = минус (101 = -1 °C)
static int8_t jk_temp(uint16_t v) { return v > 100 ? -(int8_t)(v - 100) : (int8_t)v; }

static int32_t jk_current_ma(uint16_t raw, uint8_t protocol, bool has_protocol)
{
    int32_t ca;   // в сотых долях ампера
    if (has_protocol && protocol == 0) {
        ca = (int32_t)raw - 10000;                  // старые прошивки: смещение 10000
    } else {
        ca = raw & 0x7FFF;                          // новые: старший бит = заряд
        if (!(raw & 0x8000)) ca = -ca;
    }
    return ca * 10 * (settings()->current_sign < 0 ? -1 : 1);
}

bool jk_bms_parse(const uint8_t *f, size_t len, jk_bms_data_t *out)
{
    if (len < JK_HDR_LEN + JK_TAIL_LEN || f[0] != 0x4E || f[1] != 0x57) return false;
    if ((size_t)be16(f + 2) + 2 != len) return false;
    if (f[len - 5] != 0x68) {
        ESP_LOGW(TAG, "нет маркера конца кадра");
        return false;
    }
    uint16_t crc = jk_sum(f, len - 4);
    if (crc != be16(f + len - 2)) {
        ESP_LOGW(TAG, "CRC не совпала: %04X != %04X", crc, be16(f + len - 2));
        return false;
    }

    jk_bms_data_t d = {0};
    bool have_v = false, have_i = false, have_soc = false;
    size_t i = JK_HDR_LEN;
    const size_t end = len - JK_TAIL_LEN;

    while (i < end) {
        uint8_t id = f[i++];
        const uint8_t *p = f + i;
        int n = (id == 0x79) ? (i < end ? 1 + p[0] : -1) : field_len(id);
        if (n < 0) {
            ESP_LOGD(TAG, "неизвестное поле 0x%02X (смещение %u), дальше не разбираем", id, (unsigned)(i - 1));
            break;
        }
        if (i + n > end) break;

        switch (id) {
        case 0x79: {
            int cnt = p[0] / 3;
            if (cnt > JK_MAX_CELLS) cnt = JK_MAX_CELLS;
            d.cell_count = cnt;
            for (int c = 0; c < cnt; c++) d.cell_mv[c] = be16(p + 1 + c * 3 + 1);
            break;
        }
        case 0x80: d.temp_mos = jk_temp(be16(p)); break;
        case 0x81: d.temp_t1  = jk_temp(be16(p)); break;
        case 0x82: d.temp_t2  = jk_temp(be16(p)); break;
        case 0x83: d.voltage_mv = (uint32_t)be16(p) * 10; have_v = true; break;
        case 0x84: d.current_raw = be16(p); have_i = true; break;
        case 0x85: d.soc = p[0]; have_soc = true; break;
        case 0x87: d.cycles = be16(p); break;
        case 0x8B: d.alarms = be16(p); break;
        case 0x8C: d.status = be16(p); break;
        case 0xAA: d.capacity_ah = be32(p); break;
        case 0xC0: d.protocol = p[0]; d.has_protocol = true; break;
        default: break;
        }
        i += n;
    }

    if (!have_v || !have_i || !have_soc || d.soc > 100) {
        ESP_LOGW(TAG, "в кадре нет обязательных полей");
        return false;
    }
    d.current_ma = jk_current_ma(d.current_raw, d.protocol, d.has_protocol);

    if (d.cell_count) {
        d.cell_min_mv = 0xFFFF;
        for (int c = 0; c < d.cell_count; c++) {
            if (d.cell_mv[c] < d.cell_min_mv) { d.cell_min_mv = d.cell_mv[c]; d.cell_min_num = c + 1; }
            if (d.cell_mv[c] > d.cell_max_mv) { d.cell_max_mv = d.cell_mv[c]; d.cell_max_num = c + 1; }
        }
    }
    *out = d;
    return true;
}

const char *jk_bms_alarm_name(int bit)
{
    static const char *names[] = {
        "низкая ёмкость", "перегрев MOS", "перенапряжение при заряде", "недонапряжение при разряде",
        "перегрев батареи", "перегрузка по току заряда", "перегрузка по току разряда",
        "разбаланс ячеек", "перегрев корпуса", "низкая температура батареи",
        "перенапряжение ячейки", "недонапряжение ячейки", "защита 309_A", "защита 309_B",
    };
    return (bit >= 0 && bit < (int)(sizeof(names) / sizeof(names[0]))) ? names[bit] : "неизвестная тревога";
}
