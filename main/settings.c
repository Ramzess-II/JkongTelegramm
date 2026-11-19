#include "settings.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "nvs.h"
#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef TG_BOT_TOKEN
#define TG_BOT_TOKEN ""
#endif
#ifndef TG_CHAT_IDS
#define TG_CHAT_IDS ""
#endif

static const char *TAG = "settings";

#define SET_MAGIC   0x4A4B5354u   // "JKST"
#define SET_VERSION 1
#define NVS_NS      "settings"
#define NVS_KEY     "v1"

static settings_t s_set;

const settings_t *settings(void) { return &s_set; }

int settings_parse_chats(const char *str, int64_t *out)
{
    int n = 0;
    const char *p = str;
    memset(out, 0, sizeof(int64_t) * SET_MAX_CHATS);
    while (*p && n < SET_MAX_CHATS) {
        while (*p && !(isdigit((unsigned char)*p) || *p == '-')) p++;
        if (!*p) break;
        char *end;
        long long v = strtoll(p, &end, 10);
        if (end == p) { p++; continue; }
        if (v != 0) out[n++] = v;
        p = end;
    }
    return n;
}

bool settings_chat_allowed(int64_t id)
{
    if (id == 0) return false;
    for (int i = 0; i < SET_MAX_CHATS; i++) {
        if (s_set.chat_ids[i] == id) return true;
    }
    return false;
}

void settings_defaults(settings_t *s, bool keep_network)
{
    settings_t old = *s;
    memset(s, 0, sizeof(*s));
    s->magic = SET_MAGIC;
    s->version = SET_VERSION;

    if (keep_network) {
        memcpy(s->device_name, old.device_name, sizeof(s->device_name));
        memcpy(s->wifi_ssid, old.wifi_ssid, sizeof(s->wifi_ssid));
        memcpy(s->wifi_pass, old.wifi_pass, sizeof(s->wifi_pass));
        memcpy(s->tg_token, old.tg_token, sizeof(s->tg_token));
        memcpy(s->chat_ids, old.chat_ids, sizeof(s->chat_ids));
        memcpy(s->web_pass, old.web_pass, sizeof(s->web_pass));
    } else {
        strlcpy(s->device_name, DEF_DEVICE_NAME, sizeof(s->device_name));
        strlcpy(s->wifi_ssid, WIFI_SSID, sizeof(s->wifi_ssid));
        strlcpy(s->wifi_pass, WIFI_PASS, sizeof(s->wifi_pass));
        strlcpy(s->tg_token, TG_BOT_TOKEN, sizeof(s->tg_token));
        settings_parse_chats(TG_CHAT_IDS, s->chat_ids);
        strlcpy(s->web_pass, DEF_WEB_PASSWORD, sizeof(s->web_pass));
    }
    s->web_pass[sizeof(s->web_pass) - 1] = 0;
    if (strlen(s->web_pass) < 8) strlcpy(s->web_pass, DEF_WEB_PASSWORD, sizeof(s->web_pass));

    s->cells             = DEF_CELLS;
    s->capacity_ah       = DEF_CAPACITY_AH;
    s->min_soc           = DEF_MIN_SOC;
    s->cell_off_mv       = DEF_CELL_OFF_mV;
    s->cell_on_mv        = DEF_CELL_ON_mV;
    s->cell_force_on_mv  = DEF_CELL_FORCE_ON_mV;
    s->cell_force_off_mv = DEF_CELL_FORCE_OFF_mV;
    s->night_start       = DEF_NIGHT_START_HOUR;
    s->night_end         = DEF_NIGHT_END_HOUR;
    s->reserve_h         = DEF_RESERVE_HOURS;
    s->reserve_hyst_pct  = DEF_RESERVE_HYST_PCT;
    s->default_load_w    = DEF_LOAD_W;
    s->grid_lost_a       = DEF_GRID_LOST_A;
    s->grid_back_a       = DEF_GRID_BACK_A;
    s->current_sign      = DEF_CURRENT_SIGN;
}

const char *settings_validate(const settings_t *s)
{
    if (s->cells < 1 || s->cells > 32)                     return "Число ячеек должно быть 1…32";
    if (s->cell_off_mv > 4250 || s->cell_off_mv < 3000)    return "Порог выключения: 3.00…4.25 В на ячейку";
    if (s->cell_on_mv >= s->cell_off_mv)                   return "Порог включения должен быть ниже порога выключения";
    if (s->cell_force_off_mv > s->cell_on_mv)              return "Аварийное «до» должно быть не выше порога включения";
    if (s->cell_force_on_mv >= s->cell_force_off_mv)       return "Аварийное «включить» должно быть ниже аварийного «до»";
    if (s->cell_force_on_mv < 2800)                        return "Аварийный порог слишком низкий";
    if (s->night_start > 23 || s->night_end > 23)          return "Часы ночного тарифа: 0…23";
    if (s->reserve_h > 48)                                 return "Резерв: 0…48 часов";
    if (s->min_soc > 90)                                   return "Минимальный SOC: 0…90 %";
    if (s->grid_back_a >= s->grid_lost_a)                  return "Ток «сеть вернулась» должен быть меньше тока «сеть пропала»";
    if (s->grid_lost_a <= 0 || s->grid_lost_a > 100)       return "Ток «сеть пропала»: 0…100 А";
    if (strlen(s->web_pass) < 8)                           return "Пароль веб-страницы — минимум 8 символов";
    for (const char *p = s->tg_token; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != ':' && *p != '_' && *p != '-')
            return "В токене бота недопустимые символы (лишний пробел?)";
    }
    if (s->wifi_pass[0] && strlen(s->wifi_pass) < 8)       return "Пароль Wi-Fi — минимум 8 символов (или пусто для открытой сети)";
    return NULL;
}

esp_err_t settings_save(const settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY, s, sizeof(*s));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void settings_init(void)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_set);
        ok = nvs_get_blob(h, NVS_KEY, &s_set, &sz) == ESP_OK && sz == sizeof(s_set) &&
             s_set.magic == SET_MAGIC && s_set.version == SET_VERSION;
        nvs_close(h);
    }
    if (!ok) {
        ESP_LOGW(TAG, "настроек нет, беру значения по умолчанию");
        settings_defaults(&s_set, false);
        settings_save(&s_set);
    } else if (settings_validate(&s_set)) {
        ESP_LOGW(TAG, "настройки некорректны (%s), пороги сброшены", settings_validate(&s_set));
        settings_defaults(&s_set, true);
    }
    s_set.device_name[sizeof(s_set.device_name) - 1] = 0;
    ESP_LOGI(TAG, "%s: %uS, выкл %.2f В/яч, вкл %.2f В/яч, резерв %u ч, Wi-Fi \"%s\"",
             s_set.device_name, s_set.cells, s_set.cell_off_mv / 1000.0f, s_set.cell_on_mv / 1000.0f,
             s_set.reserve_h, s_set.wifi_ssid);
}
