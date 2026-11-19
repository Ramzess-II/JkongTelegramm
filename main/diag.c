#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "hal/uart_hal.h"
#include "soc/uart_struct.h"

#define DIAG_MAGIC     0x44494147u   // "DIAG"
#define DIAG_RING      4096
#define INFLIGHT_MAGIC 0x494E464Cu   // "INFL"

typedef struct {
    uint32_t magic;
    uint32_t total;                  // сколько байт записано всего (позиция = total % DIAG_RING)
    char     ring[DIAG_RING];
    uint32_t if_magic;
    int64_t  if_id;
    char     if_text[64];
} diag_rtc_t;

static RTC_NOINIT_ATTR diag_rtc_t s_rtc;

static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_reset_reason_t s_reason;
static char              *s_prev = "";
static bool               s_inflight_crashed;
static int64_t            s_crash_id;
static char               s_crash_text[64];
static uint8_t            s_esc;     // пропуск цветовых ESC-последовательностей

static inline void IRAM_ATTR ring_put(char c)
{
    if (s_esc) {
        if (c == 'm') s_esc = 0;
        return;
    }
    if (c == 0x1B) { s_esc = 1; return; }
    if (c == '\r') return;
    s_rtc.ring[s_rtc.total % DIAG_RING] = c;
    s_rtc.total++;
}

// Линейная копия кольца (старое → новое)
static char *ring_dup(const diag_rtc_t *r)
{
    uint32_t n = r->total < DIAG_RING ? r->total : DIAG_RING;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    uint32_t start = r->total < DIAG_RING ? 0 : r->total % DIAG_RING;
    for (uint32_t i = 0; i < n; i++) {
        char c = r->ring[(start + i) % DIAG_RING];
        out[i] = (c == '\n' || c == '\t' || (unsigned char)c >= 0x20) ? c : '?';
    }
    out[n] = 0;
    return out;
}

// ------------------------------------------------------------------ перехват паники

void __real_uart_hal_write_txfifo(uart_hal_context_t *hal, const uint8_t *buf, uint32_t data_size, uint32_t *write_size);

void IRAM_ATTR __wrap_uart_hal_write_txfifo(uart_hal_context_t *hal, const uint8_t *buf, uint32_t data_size,
                                            uint32_t *write_size)
{
    __real_uart_hal_write_txfifo(hal, buf, data_size, write_size);
    // На консольный UART0 через HAL пишет только обработчик паники. Блокировку не берём:
    // в панике второе ядро остановлено, а спинлок мог остаться занятым.
    if (hal && hal->dev == &UART0 && s_rtc.magic == DIAG_MAGIC) {
        const uint32_t n = write_size ? *write_size : data_size;
        for (uint32_t i = 0; i < n; i++) ring_put((char)buf[i]);
    }
}

// ------------------------------------------------------------------ перехват обычного лога

static int diag_vprintf(const char *fmt, va_list ap)
{
    char b[200];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(b, sizeof(b), fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        if (n >= (int)sizeof(b)) {
            n = sizeof(b) - 1;
            b[n - 1] = '\n';
        }
        portENTER_CRITICAL_SAFE(&s_mux);
        for (int i = 0; i < n; i++) ring_put(b[i]);
        portEXIT_CRITICAL_SAFE(&s_mux);
    }
    return vprintf(fmt, ap);
}

// ------------------------------------------------------------------ API

void diag_init(void)
{
    s_reason = esp_reset_reason();
    const bool valid = s_rtc.magic == DIAG_MAGIC && s_reason != ESP_RST_POWERON;

    if (valid) {
        char *p = ring_dup(&s_rtc);
        if (p) s_prev = p;
        if (s_rtc.if_magic == INFLIGHT_MAGIC && diag_reset_abnormal()) {
            s_inflight_crashed = true;
            s_crash_id = s_rtc.if_id;
            memcpy(s_crash_text, s_rtc.if_text, sizeof(s_crash_text));
            s_crash_text[sizeof(s_crash_text) - 1] = 0;
        }
    }
    s_rtc.if_magic = 0;
    s_rtc.total = 0;
    s_esc = 0;
    s_rtc.magic = DIAG_MAGIC;
    esp_log_set_vprintf(diag_vprintf);
}

esp_reset_reason_t diag_reset_reason(void) { return s_reason; }

bool diag_reset_abnormal(void)
{
    switch (s_reason) {
    case ESP_RST_PANIC: case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT:
    case ESP_RST_BROWNOUT: case ESP_RST_CPU_LOCKUP: case ESP_RST_PWR_GLITCH:
        return true;
    default:
        return false;
    }
}

const char *diag_reset_reason_str(void)
{
    switch (s_reason) {
    case ESP_RST_POWERON:    return "включение питания";
    case ESP_RST_EXT:        return "внешний сброс (кнопка RESET)";
    case ESP_RST_SW:         return "программная перезагрузка";
    case ESP_RST_PANIC:      return "ПАНИКА — сбой прошивки";
    case ESP_RST_INT_WDT:    return "watchdog прерываний";
    case ESP_RST_TASK_WDT:   return "watchdog задач (зависание)";
    case ESP_RST_WDT:        return "watchdog";
    case ESP_RST_BROWNOUT:   return "ПРОСАДКА ПИТАНИЯ (brownout)";
    case ESP_RST_DEEPSLEEP:  return "выход из сна";
    case ESP_RST_USB:        return "сброс по USB";
    case ESP_RST_JTAG:       return "сброс по JTAG";
    case ESP_RST_PWR_GLITCH: return "сбой питания (glitch)";
    case ESP_RST_CPU_LOCKUP: return "зависание CPU";
    default:                 return "неизвестна";
    }
}

const char *diag_prev_log(void) { return s_prev; }

char *diag_cur_log_dup(void)
{
    // Кольцо меняется из других задач — сначала копия под блокировкой
    diag_rtc_t *tmp = malloc(sizeof(*tmp));
    if (!tmp) return NULL;
    portENTER_CRITICAL(&s_mux);
    memcpy(tmp, &s_rtc, sizeof(*tmp));
    portEXIT_CRITICAL(&s_mux);
    char *out = ring_dup(tmp);
    free(tmp);
    return out;
}

bool diag_prev_backtrace(char *out, size_t size)
{
    const char *p = strstr(s_prev, "Backtrace:");
    if (!p) return false;
    size_t n = strcspn(p, "\n");
    if (n >= size) n = size - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

void diag_inflight_set(int64_t update_id, const char *text)
{
    s_rtc.if_id = update_id;
    strlcpy(s_rtc.if_text, text ? text : "", sizeof(s_rtc.if_text));
    s_rtc.if_magic = INFLIGHT_MAGIC;
}

void diag_inflight_clear(void) { s_rtc.if_magic = 0; }

bool diag_inflight_crashed(int64_t *update_id, char *text, size_t size)
{
    if (!s_inflight_crashed) return false;
    if (update_id) *update_id = s_crash_id;
    if (text) strlcpy(text, s_crash_text, size);
    return true;
}
