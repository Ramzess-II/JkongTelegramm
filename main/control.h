#pragma once
/*
 * Основной цикл: раз в секунду опрос BMS -> детектор сети -> профиль потребления ->
 * решение по реле заряда -> уведомления. Telegram читает снимок состояния через
 * control_get_status() и меняет режим через control_set_mode().
 */
#include <stdint.h>
#include <stdbool.h>
#include "jk_bms.h"

typedef enum { GRID_UNKNOWN, GRID_ON, GRID_OFF } grid_state_t;
typedef enum { CHG_NONE, CHG_NIGHT, CHG_RESERVE, CHG_FORCE, CHG_MANUAL } chg_reason_t;
typedef enum { MODE_AUTO, MODE_MANUAL_ON, MODE_MANUAL_OFF } chg_mode_t;

typedef struct {
    jk_bms_data_t bms;
    bool          bms_ever;         // BMS хоть раз ответила
    bool          bms_ok;           // данные свежие
    int64_t       bms_last_ok_s;

    grid_state_t  grid;
    int64_t       grid_since_s;     // когда сменилось состояние сети (монотонное время)

    bool          relay_on;         // решение логики заряда
    bool          relay_pin;        // что реально выставлено на пине (с учётом теста)
    int           relay_test_left_s;// >0 — идёт тестовое включение с веб-страницы
    chg_reason_t  reason;
    bool          full_latched;     // дошли до CHG_OFF_mV, ждём падения до CHG_RESTART_mV
    chg_mode_t    mode;
    int64_t       mode_until_s;

    float         capacity_ah;
    float         load_w;           // текущая нагрузка от батареи
    float         avail_wh;         // доступная энергия (выше BATTERY_MIN_SOC)
    float         reserve_wh;       // нужно на RESERVE_HOURS
    float         runtime_h;        // прогноз по профилю
    float         outage_wh;        // израсходовано за текущее/последнее отключение
    uint8_t       outage_soc_start;

    bool          time_valid;
    int64_t       now_s;            // монотонное время снимка
} app_status_t;

void        control_start(void);
void        control_get_status(app_status_t *out);
void        control_set_mode(chg_mode_t mode, int hours);
void        control_reset_profile(void);
// Тестовое включение реле на seconds (1..600), 0 — выключить тест.
// Работает и без BMS, но не при пропаже сети и не при заряженной батарее.
// Возвращает NULL или текст, почему нельзя.
const char *control_relay_test(int seconds);
// Почему заряд сейчас нельзя включить (failsafe/заряжена); NULL — можно
const char *control_block_reason(const app_status_t *s);
const char *control_reason_str(chg_reason_t r);
int64_t     control_now_s(void);
