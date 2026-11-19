#pragma once
/*
 * Настройки, которые меняются с веб-страницы. Хранятся в NVS одним блоком.
 * Во время работы только читаются: сохранение новых настроек -> перезагрузка.
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SET_MAX_CHATS 4

typedef struct {
    uint32_t magic;
    uint32_t version;

    char     device_name[32];
    char     wifi_ssid[33];
    char     wifi_pass[65];
    char     tg_token[64];
    int64_t  chat_ids[SET_MAX_CHATS];   // 0 = пусто
    char     web_pass[33];

    uint8_t  cells;
    uint16_t capacity_ah;               // 0 = из BMS
    uint8_t  min_soc;
    uint16_t cell_off_mv, cell_on_mv, cell_force_on_mv, cell_force_off_mv;
    uint8_t  night_start, night_end;
    uint8_t  reserve_h;
    uint8_t  reserve_hyst_pct;
    uint16_t default_load_w;
    float    grid_lost_a, grid_back_a;
    int8_t   current_sign;
} settings_t;

void              settings_init(void);
const settings_t *settings(void);
void              settings_defaults(settings_t *s, bool keep_network);
esp_err_t         settings_save(const settings_t *s);
const char       *settings_validate(const settings_t *s);   // NULL = ок, иначе текст ошибки
int               settings_parse_chats(const char *str, int64_t *out);   // "1,2,-100" -> кол-во
bool              settings_chat_allowed(int64_t id);

// Пороги в мВ на весь пакет
static inline uint32_t set_pack_mv(uint16_t cell_mv) { return (uint32_t)cell_mv * settings()->cells; }
