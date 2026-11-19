#include "energy.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "config.h"
#include "settings.h"

#define DEFAULT_LOAD_W (settings()->default_load_w)

static const char *TAG = "energy";

#define PROFILE_MAGIC  0x50524F31u   // "PRO1"
#define NVS_NS         "energy"
#define NVS_KEY        "profile"

typedef struct {
    uint32_t magic;
    float    hour_w[24];
    uint8_t  hour_known[24];
    float    mean_w;
    uint8_t  mean_known;
} profile_t;

static profile_t s_p;

// Накопление за текущий час (или за текущий "слот" в 1 час, если время неизвестно)
static int   s_bucket_hour = -2;
static float s_bucket_s;        // сколько секунд прошло в слоте
static float s_acc_s;           // из них работали от батареи
static double s_acc_ws;         // энергия от батареи, Вт*с

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, &s_p, sizeof(s_p));
    nvs_commit(h);
    nvs_close(h);
}

void energy_init(void)
{
    memset(&s_p, 0, sizeof(s_p));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_p);
        if (nvs_get_blob(h, NVS_KEY, &s_p, &sz) != ESP_OK || sz != sizeof(s_p) || s_p.magic != PROFILE_MAGIC) {
            memset(&s_p, 0, sizeof(s_p));
        }
        nvs_close(h);
    }
    s_p.magic = PROFILE_MAGIC;
    int known = 0;
    for (int i = 0; i < 24; i++) known += s_p.hour_known[i];
    ESP_LOGI(TAG, "профиль загружен: известно %d из 24 часов, среднее %.0f Вт", known,
             s_p.mean_known ? s_p.mean_w : (float)DEFAULT_LOAD_W);
}

void energy_reset(void)
{
    memset(&s_p, 0, sizeof(s_p));
    s_p.magic = PROFILE_MAGIC;
    save();
}

static float ema(float old, bool known, float val, float a)
{
    return known ? old * (1.0f - a) + val * a : val;
}

static void finalize_bucket(int hour)
{
    if (s_acc_s < PROFILE_MIN_COVERAGE_S) return;
    float avg = (float)(s_acc_ws / s_acc_s);
    float cover = s_acc_s / 3600.0f;
    if (cover > 1.0f) cover = 1.0f;
    float a = PROFILE_EMA_ALPHA * cover;      // неполный час влияет слабее

    if (hour >= 0 && hour < 24) {
        s_p.hour_w[hour] = ema(s_p.hour_w[hour], s_p.hour_known[hour], avg, a);
        s_p.hour_known[hour] = 1;
    }
    s_p.mean_w = ema(s_p.mean_w, s_p.mean_known, avg, a);
    s_p.mean_known = 1;
    save();
    ESP_LOGI(TAG, "час %d: средняя нагрузка %.0f Вт за %.0f мин -> профиль %.0f Вт",
             hour, avg, s_acc_s / 60, hour >= 0 ? s_p.hour_w[hour] : s_p.mean_w);
}

void energy_tick(bool on_battery, float load_w, float dt_s, int hour)
{
    if (dt_s <= 0 || dt_s > 10) dt_s = 1;   // защита от скачков времени

    // Новый слот: сменился час, или время неизвестно и прошёл час
    if (hour != s_bucket_hour || (hour < 0 && s_bucket_s >= 3600)) {
        if (s_bucket_hour != -2) finalize_bucket(s_bucket_hour);
        s_bucket_hour = hour;
        s_bucket_s = 0;
        s_acc_s = 0;
        s_acc_ws = 0;
    }
    s_bucket_s += dt_s;
    if (on_battery) {
        s_acc_s += dt_s;
        s_acc_ws += (double)load_w * dt_s;
    }
}

bool  energy_hour_known(int hour) { return hour >= 0 && hour < 24 && s_p.hour_known[hour]; }
bool  energy_mean_known(void)     { return s_p.mean_known; }
float energy_mean_w(void)         { return s_p.mean_known ? s_p.mean_w : (float)DEFAULT_LOAD_W; }

float energy_hour_w(int hour)
{
    if (energy_hour_known(hour)) return s_p.hour_w[hour];
    return energy_mean_w();
}

float energy_need_wh(int hour, int minute, float hours)
{
    if (hour < 0) return hours * energy_mean_w();
    float need = 0, left = hours;
    float seg = (60 - minute) / 60.0f;   // остаток текущего часа
    int h = hour;
    while (left > 0) {
        float t = left < seg ? left : seg;
        need += t * energy_hour_w(h);
        left -= t;
        h = (h + 1) % 24;
        seg = 1.0f;
    }
    return need;
}

float energy_runtime_h(float avail_wh, int hour, int minute, float max_h)
{
    if (avail_wh <= 0) return 0;
    if (hour < 0) {
        float r = avail_wh / energy_mean_w();
        return r > max_h ? max_h : r;
    }
    float t = 0;
    float seg = (60 - minute) / 60.0f;
    int h = hour;
    while (t < max_h) {
        float p = energy_hour_w(h);
        if (p < 1) p = 1;
        float e = seg * p;
        if (e >= avail_wh) return t + avail_wh / p;
        avail_wh -= e;
        t += seg;
        h = (h + 1) % 24;
        seg = 1.0f;
    }
    return max_h;
}
