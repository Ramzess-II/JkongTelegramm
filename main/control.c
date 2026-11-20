#include "control.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "config.h"
#include "energy.h"
#include "telegram.h"
#include "util.h"
#include "settings.h"
#include "ota.h"
#include "diag.h"

static const char *TAG = "control";

// Пороги из настроек (на ячейку) -> на весь пакет
#define CFG            (settings())
#define CHG_OFF_mV     set_pack_mv(CFG->cell_off_mv)
#define CHG_ON_mV      set_pack_mv(CFG->cell_on_mv)
#define FORCE_ON_mV    set_pack_mv(CFG->cell_force_on_mv)
#define FORCE_OFF_mV   set_pack_mv(CFG->cell_force_off_mv)
#define NOMINAL_V      (CFG->cells * 3.7f)

static SemaphoreHandle_t s_mux;
static app_status_t      s_pub;            // снимок для чтения из других задач

// Запросы из Telegram (под s_mux)
static bool       s_mode_req;
static chg_mode_t s_req_mode;
static int64_t    s_req_until_s;
static bool       s_profile_reset_req;
static bool       s_test_req;
static int64_t    s_test_req_until_s;

#define RELAY_TEST_MAX_S 600

int64_t control_now_s(void) { return esp_timer_get_time() / 1000000; }

const char *control_reason_str(chg_reason_t r)
{
    switch (r) {
    case CHG_NIGHT:   return "ночной тариф";
    case CHG_RESERVE: return "мало резерва на отключение";
    case CHG_FORCE:   return "аварийно низкое напряжение";
    case CHG_MANUAL:  return "ручной режим";
    default:          return "—";
    }
}

void control_get_status(app_status_t *out)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_pub;
    xSemaphoreGive(s_mux);
}

void control_set_mode(chg_mode_t mode, int hours)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_req_mode = mode;
    s_req_until_s = mode == MODE_AUTO ? 0 : control_now_s() + (int64_t)hours * 3600;
    s_mode_req = true;
    xSemaphoreGive(s_mux);
}

void control_reset_profile(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_profile_reset_req = true;
    xSemaphoreGive(s_mux);
}

const char *control_block_reason(const app_status_t *s)
{
    if (!s->bms_ever) return "BMS ни разу не ответила — без данных BMS заряд не включается (проверьте UART)";
    if (!s->bms_ok)   return "BMS не отвечает — без данных BMS заряд не включается";
    if (s->grid == GRID_OFF)
        return "сети нет (работаем от инвертора) — включится, когда сеть появится";
    if (s->grid == GRID_UNKNOWN)
        return "наличие сети ещё определяется (до ~30 с после запуска или после появления связи с BMS)";
    if (s->bms.voltage_mv >= set_pack_mv(CFG->cell_off_mv)) return "батарея уже заряжена до порога «выключить»";
    return NULL;
}

const char *control_relay_test(int seconds)
{
    if (seconds < 0) seconds = 0;
    if (seconds > RELAY_TEST_MAX_S) seconds = RELAY_TEST_MAX_S;
    const char *err = NULL;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (seconds > 0 && s_pub.grid == GRID_OFF) {
        err = "Сети нет (работаем от инвертора) — включать заряд нельзя";
    } else if (seconds > 0 && s_pub.bms_ok && s_pub.bms.voltage_mv >= set_pack_mv(CFG->cell_off_mv)) {
        err = "Батарея заряжена до порога «выключить» — включать заряд нельзя";
    } else {
        s_test_req_until_s = seconds ? control_now_s() + seconds : 0;
        s_test_req = true;
    }
    xSemaphoreGive(s_mux);
    return err;
}

static void relay_set(bool on)
{
    gpio_set_level(RELAY_PIN, on ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
}

static bool is_night(int h)
{
    const int a = CFG->night_start, b = CFG->night_end;
    if (h < 0 || a == b) return false;           // начало = конец -> ночного тарифа нет
    if (a > b) return h >= a || h < b;
    return h >= a && h < b;
}

// Тревоги BMS с защитой от «дребезга». Тревога может мигать десятки раз в час (ячейка дошла до порога,
// BMS отсекла заряд, напряжение просело, тревога снялась…) — раньше это давало сотни сообщений за ночь.
//  - биты из ALARM_SILENT_MASK в Telegram не отправляются вовсе (видны в /status);
//  - одна и та же тревога — не чаще раза в ALARM_REPEAT_S;
//  - «снята» — только если тревога пропала на ALARM_CLEAR_S и о её появлении сообщали.
static void alarms_tick(uint16_t alarms, int64_t now)
{
    static uint16_t active;              // тревоги с учётом задержки снятия
    static uint16_t reported;            // о появлении сообщили в текущем эпизоде
    static int64_t  last_sent[16];
    static int64_t  gone_since[16];
    static bool     init;
    if (!init) {
        for (int b = 0; b < 16; b++) last_sent[b] = INT64_MIN / 2;
        init = true;
    }

    char added[256] = "", cleared[256] = "";
    for (int b = 0; b < 16; b++) {
        const uint16_t bit = 1u << b;
        if (alarms & bit) {
            gone_since[b] = 0;
            if (active & bit) continue;
            active |= bit;
            if ((ALARM_SILENT_MASK & bit) || now - last_sent[b] < ALARM_REPEAT_S) continue;
            ESP_LOGW(TAG, "тревога BMS: %s", jk_bms_alarm_name(b));
            last_sent[b] = now;
            reported |= bit;
            strappend(added, sizeof(added), "\n• %s", jk_bms_alarm_name(b));
        } else if (active & bit) {
            if (!gone_since[b]) gone_since[b] = now;
            if (now - gone_since[b] < ALARM_CLEAR_S) continue;
            active &= ~bit;
            if (reported & bit) {
                ESP_LOGI(TAG, "тревога BMS снята: %s", jk_bms_alarm_name(b));
                reported &= ~bit;
                strappend(cleared, sizeof(cleared), "\n• %s", jk_bms_alarm_name(b));
            }
        }
    }
    if (added[0])   tg_notify("🚨 Тревога BMS:%s", added);
    if (cleared[0]) tg_notify("✅ Тревога BMS снята:%s", cleared);
}

static void control_task(void *arg)
{
    esp_task_wdt_add(NULL);

    app_status_t st = {0};
    st.grid = GRID_UNKNOWN;
    st.grid_since_s = control_now_s();
    st.bms_last_ok_s = -1000000;

    int64_t      relay_switched_s = -1000000;
    int64_t      test_until_s = 0;              // тестовое включение реле с веб-страницы
    grid_state_t grid_cand = GRID_UNKNOWN;
    int64_t      grid_cand_since = 0;
    bool         bms_lost_sent = false, bms_never_sent = false, boot_sent = false;
    bool         warn_rt_sent = false, warn_soc_sent = false;
    int64_t      last_tick_us = esp_timer_get_time();
    const int64_t boot_s = control_now_s();

    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        esp_task_wdt_reset();
        const int64_t now_us = esp_timer_get_time();
        const float   dt = (now_us - last_tick_us) / 1e6f;
        last_tick_us = now_us;
        const int64_t now = now_us / 1000000;

        // ---------- Запросы из Telegram ----------
        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (s_mode_req) {
            st.mode = s_req_mode;
            st.mode_until_s = s_req_until_s;
            s_mode_req = false;
        }
        bool reset_profile = s_profile_reset_req;
        s_profile_reset_req = false;
        if (s_test_req) {
            test_until_s = s_test_req_until_s;
            s_test_req = false;
            if (test_until_s) ESP_LOGW(TAG, "тест реле: ВКЛ на %lld с", (long long)(test_until_s - now));
            else              ESP_LOGW(TAG, "тест реле: остановлен с веб-страницы");
        }
        xSemaphoreGive(s_mux);
        if (reset_profile) energy_reset();
        if (st.mode != MODE_AUTO && now >= st.mode_until_s) {
            st.mode = MODE_AUTO;
            tg_send("⏱ Ручной режим закончился, заряд снова в автомате");
        }

        // ---------- Время ----------
        time_t t = time(NULL);
        struct tm lt;
        localtime_r(&t, &lt);
        st.time_valid = lt.tm_year >= (2024 - 1900);
        const int hour = st.time_valid ? lt.tm_hour : -1;
        const int minute = st.time_valid ? lt.tm_min : 0;

        // ---------- BMS ----------
        jk_bms_data_t d;
        if (jk_bms_poll(&d) == ESP_OK) {
            st.bms = d;
            st.bms_last_ok_s = now;
            st.bms_ever = true;
        }
        const bool was_ok = st.bms_ok;
        st.bms_ok = st.bms_ever && (now - st.bms_last_ok_s) <= BMS_TIMEOUT_S;

        if (was_ok && !st.bms_ok && !bms_lost_sent) {
            tg_send("⚠️ BMS не отвечает! Заряд выключен до восстановления связи.");
            bms_lost_sent = true;
        } else if (st.bms_ok && bms_lost_sent) {
            tg_send("✅ Связь с BMS восстановлена");
            bms_lost_sent = false;
        }
        if (!st.bms_ever && !bms_never_sent && now - boot_s > 60) {
            tg_send("⚠️ BMS не ответила ни разу с момента запуска. Проверьте подключение UART (TX/RX, GND).");
            bms_never_sent = true;
        }

        if (st.bms_ok) {
            alarms_tick(st.bms.alarms, now);

            const float v = st.bms.voltage_mv / 1000.0f;
            const float discharge_a = st.bms.current_ma < 0 ? -st.bms.current_ma / 1000.0f : 0.0f;
            st.load_w = v * discharge_a;

            // ---------- Детектор сети ----------
            grid_state_t observed;
            if (discharge_a >= CFG->grid_lost_a)      observed = GRID_OFF;
            else if (discharge_a <= CFG->grid_back_a) observed = GRID_ON;
            else observed = (st.grid == GRID_UNKNOWN) ? GRID_ON : st.grid;   // между порогами — без изменений

            if (observed == st.grid) {
                grid_cand = st.grid;
            } else {
                if (grid_cand != observed) { grid_cand = observed; grid_cand_since = now; }
                const int delay = observed == GRID_OFF ? GRID_LOST_DELAY_S : GRID_BACK_DELAY_S;
                if (now - grid_cand_since >= delay) {
                    const grid_state_t prev = st.grid;
                    const int64_t lasted = now - st.grid_since_s;
                    st.grid = observed;
                    st.grid_since_s = now;
                    char dur[32];
                    fmt_duration(lasted, dur, sizeof(dur));

                    if (observed == GRID_OFF) {
                        st.outage_wh = 0;
                        st.outage_soc_start = st.bms.soc;
                        warn_rt_sent = warn_soc_sent = false;
                    }
                    if (prev != GRID_UNKNOWN) {
                        if (observed == GRID_OFF) {
                            float rt = energy_runtime_h(st.avail_wh, hour, minute, 72);
                            char rts[32];
                            fmt_duration((int64_t)(rt * 3600), rts, sizeof(rts));
                            tg_notify("⚡ Свет пропал (был %s).\n🔋 %.2f В · %u%%\n⏳ По профилю хватит примерно на %s",
                                      dur, v, st.bms.soc, rts);
                        } else {
                            tg_notify("💡 Свет появился. Не было %s.\nИзрасходовано %.2f кВт·ч, заряд %u%% → %u%%",
                                      dur, st.outage_wh / 1000.0f, st.outage_soc_start, st.bms.soc);
                        }
                    }
                }
            }

            // ---------- Энергия и прогноз ----------
            const bool on_battery = st.grid == GRID_OFF;
            energy_tick(on_battery, st.load_w, dt, hour);
            if (on_battery) st.outage_wh += st.load_w * dt / 3600.0f;

            st.capacity_ah = CFG->capacity_ah > 0 ? (float)CFG->capacity_ah : (float)st.bms.capacity_ah;
            const int soc_above = st.bms.soc > CFG->min_soc ? st.bms.soc - CFG->min_soc : 0;
            st.avail_wh = soc_above / 100.0f * st.capacity_ah * NOMINAL_V;
            st.reserve_wh = energy_need_wh(hour, minute, CFG->reserve_h);
            st.runtime_h = energy_runtime_h(st.avail_wh, hour, minute, 72);

            if (on_battery) {
                if (!warn_rt_sent && st.capacity_ah > 0 && st.runtime_h < WARN_RUNTIME_H) {
                    char rts[32];
                    fmt_duration((int64_t)(st.runtime_h * 3600), rts, sizeof(rts));
                    tg_notify("🪫 Заряда осталось примерно на %s (%u%%, %.2f В)", rts, st.bms.soc, v);
                    warn_rt_sent = true;
                }
                if (!warn_soc_sent && st.bms.soc <= WARN_SOC) {
                    tg_notify("🪫 Заряд батареи %u%% (%.2f В)", st.bms.soc, v);
                    warn_soc_sent = true;
                }
            }
        } else {
            st.load_w = 0;
        }

        // Сообщение о старте — когда стало понятно состояние сети (или через минуту)
        if (!boot_sent && (st.grid != GRID_UNKNOWN || now - boot_s > 60)) {
            if (diag_reset_abnormal()) {
                char bt[200] = "";
                diag_prev_backtrace(bt, sizeof(bt));
                tg_notify("⚠️ Прошлая перезагрузка: %s.%s%s\nПолный лог: веб-страница → /log",
                          diag_reset_reason_str(), bt[0] ? "\n" : "", bt);
            }
            if (st.bms_ok) {
                tg_notify("✅ Устройство запущено (v%s).\n%s\n🔋 %.2f В · %u%%", ota_version(),
                          st.grid == GRID_OFF ? "⚡ Сети нет, работаем от батареи" : "🔌 Сеть есть",
                          st.bms.voltage_mv / 1000.0f, st.bms.soc);
            } else {
                tg_notify("✅ Устройство запущено (v%s, BMS пока не отвечает)", ota_version());
            }
            boot_sent = true;
        }
        ota_tick(st.bms_ok);   // подтверждение новой прошивки или откат

        // ---------- Решение по реле ----------
        bool want = false, urgent = false;
        chg_reason_t reason = CHG_NONE;
        const char *off_why = NULL;
        const uint32_t vmv = st.bms.voltage_mv;

        if (!st.bms_ok) {
            urgent = true;                                   // отказоустойчивость: нет данных — не заряжаем
        } else if (st.grid != GRID_ON) {
            urgent = true;                                   // от инвертора заряжать нельзя
        } else {
            if (vmv >= CHG_OFF_mV)          st.full_latched = true;
            else if (vmv <= CHG_ON_mV)  st.full_latched = false;

            if (st.mode == MODE_MANUAL_OFF) {
                urgent = true;
                off_why = "ручное выключение";
            } else if (vmv >= CHG_OFF_mV) {
                urgent = true;
                off_why = "батарея заряжена";
                if (st.mode == MODE_MANUAL_ON) {
                    st.mode = MODE_AUTO;
                    tg_send("🔋 Заряжено до порога, ручной режим снят");
                }
            } else if (st.mode == MODE_MANUAL_ON) {
                want = true;
                reason = CHG_MANUAL;
            } else {
                const bool on = st.relay_on;
                const bool night = st.time_valid && is_night(hour);
                const bool reserve_ok = st.capacity_ah > 0;  // без ёмкости резерв не посчитать
                bool force = on ? (vmv < FORCE_OFF_mV) : (vmv <= FORCE_ON_mV);
                bool reserve = reserve_ok && CFG->reserve_h > 0 &&
                               st.avail_wh < st.reserve_wh * (on ? 1.0f + CFG->reserve_hyst_pct / 100.0f : 1.0f);
                bool night_chg = night && (on || vmv <= CHG_ON_mV);
                if (st.full_latched && !on) { reserve = false; night_chg = false; }

                if (force)          reason = CHG_FORCE;
                else if (reserve)   reason = CHG_RESERVE;
                else if (night_chg) reason = CHG_NIGHT;
                want = reason != CHG_NONE;
                if (!want) {
                    switch (st.reason) {
                    case CHG_NIGHT:  off_why = "закончился ночной тариф"; break;
                    case CHG_MANUAL: off_why = "ручной режим закончился"; break;
                    case CHG_FORCE:  off_why = "напряжение поднялось выше аварийного"; break;
                    default:         off_why = "резерв набран"; break;
                    }
                }
            }
        }

        if (want != st.relay_on) {
            if ((!want && urgent) || now - relay_switched_s >= RELAY_MIN_SWITCH_S) {
                st.relay_on = want;
                relay_switched_s = now;
                ESP_LOGI(TAG, "реле %s (%s)", want ? "ВКЛ" : "ВЫКЛ", want ? control_reason_str(reason) : (off_why ? off_why : "-"));
#if NOTIFY_RELAY
                if (want) {
                    tg_notify("🔌 Заряд включён: %s (%.2f В, %u%%)", control_reason_str(reason), vmv / 1000.0f, st.bms.soc);
                } else if (off_why) {   // при пропаже сети/BMS уже есть отдельное сообщение
                    tg_notify("🔋 Заряд выключен: %s (%.2f В, %u%%)", off_why, vmv / 1000.0f, st.bms.soc);
                }
#endif
            }
        }
        if (st.relay_on && want) st.reason = reason;
        if (!st.relay_on) st.reason = CHG_NONE;

        // ---------- Пин реле: решение логики или тест с веб-страницы ----------
        bool test = test_until_s && now < test_until_s;
        if (test && (st.grid == GRID_OFF || (st.bms_ok && vmv >= CHG_OFF_mV))) {
            ESP_LOGW(TAG, "тест реле прерван: %s", st.grid == GRID_OFF ? "пропала сеть" : "батарея заряжена");
            test = false;
        }
        if (!test && test_until_s) {
            test_until_s = 0;
            ESP_LOGW(TAG, "тест реле закончен, реле по логике: %s", st.relay_on ? "ВКЛ" : "ВЫКЛ");
        }
        const bool pin = test || st.relay_on;
        if (pin != st.relay_pin) {
            relay_set(pin);
            st.relay_pin = pin;
        }
        st.relay_test_left_s = test ? (int)(test_until_s - now) : 0;

        // ---------- Публикация снимка ----------
        st.now_s = now;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        s_pub = st;
        xSemaphoreGive(s_mux);

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(BMS_POLL_MS));
    }
}

void control_start(void)
{
    // Реле выключено с самого старта, до любых решений
    gpio_set_level(RELAY_PIN, !RELAY_ACTIVE_LEVEL);
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << RELAY_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    relay_set(false);

    s_mux = xSemaphoreCreateMutex();
    s_pub.grid = GRID_UNKNOWN;
    ESP_ERROR_CHECK(jk_bms_init());
    xTaskCreate(control_task, "control", 6144, NULL, 6, NULL);
}
