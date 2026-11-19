#pragma once
/*
 * JK BMS, протокол "NW" (0x4E 0x57) по TTL UART (разъём GPS/UART на BMS).
 * Ответ разбирается по ID полей, а не по фиксированным смещениям,
 * поэтому не зависит от количества ячеек.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define JK_MAX_CELLS 32

typedef struct {
    uint8_t  cell_count;
    uint16_t cell_mv[JK_MAX_CELLS];
    uint16_t cell_min_mv, cell_max_mv;
    uint8_t  cell_min_num, cell_max_num;  // номера ячеек, с 1
    uint32_t voltage_mv;                  // общее напряжение (0x83)
    int32_t  current_ma;                  // + заряд, - разряд (0x84)
    uint16_t current_raw;
    uint8_t  soc;                         // % (0x85)
    int8_t   temp_mos, temp_t1, temp_t2;  // °C (0x80..0x82)
    uint16_t cycles;                      // (0x87)
    uint32_t capacity_ah;                 // ёмкость из настроек BMS (0xAA), 0 если нет
    uint16_t alarms;                      // биты тревог (0x8B)
    uint16_t status;                      // (0x8C)
    uint8_t  protocol;                    // версия протокола (0xC0)
    bool     has_protocol;
} jk_bms_data_t;

// Диагностика связи — для проверки пайки на веб-странице
typedef enum {
    JK_RES_NONE = 0,     // опроса ещё не было
    JK_RES_OK,           // кадр принят и разобран
    JK_RES_NO_REPLY,     // ни одного байта в ответ
    JK_RES_GARBAGE,      // байты есть, но заголовка 4E 57 нет
    JK_RES_BAD_FRAME,    // заголовок есть, но кадр битый/оборван/не разобран
    JK_RES_ECHO,         // пришёл наш же запрос — TX и RX замкнуты
} jk_result_t;

#define JK_DIAG_RAW 32

typedef struct {
    uint32_t    polls, ok, no_reply, garbage, bad_frame, echo;
    jk_result_t last;
    uint16_t    last_bytes;            // сколько байт пришло в последнем опросе
    uint8_t     raw[JK_DIAG_RAW];      // первые байты последнего ответа
    uint8_t     raw_len;
} jk_bms_diag_t;

esp_err_t   jk_bms_init(void);
void        jk_bms_get_diag(jk_bms_diag_t *out);
esp_err_t   jk_bms_poll(jk_bms_data_t *out);
bool        jk_bms_parse(const uint8_t *frame, size_t len, jk_bms_data_t *out);
const char *jk_bms_alarm_name(int bit);
