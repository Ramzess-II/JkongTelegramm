#include "ota.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "settings.h"
#include "control.h"
#include "telegram.h"
#include "net.h"

static const char *TAG = "ota";

// Что было до обновления: запоминается в NVS перед перезагрузкой в новую прошивку.
typedef struct {
    char    label[17];      // слот, в который записали новую прошивку
    char    ver[32];        // её версия
    char    from_ver[32];   // версия, с которой обновлялись
    uint8_t need_bms;       // BMS отвечала до обновления — должна отвечать и после
    uint8_t need_wifi;
    uint8_t need_tg;
} pend_t;

#define NVS_NS  "ota"
#define NVS_KEY "pend"

static atomic_bool s_busy;
static bool   s_pending;    // работаем «на пробу», ждём подтверждения
static pend_t s_pend;

// ------------------------------------------------------------------ версия

const char *ota_version(void)    { return esp_app_get_description()->version; }
const char *ota_build_date(void) { return esp_app_get_description()->date; }

// ------------------------------------------------------------------ NVS

static bool pend_load(pend_t *p)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*p);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, p, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*p)) return false;
    p->label[sizeof(p->label) - 1] = 0;
    p->ver[sizeof(p->ver) - 1] = 0;
    p->from_ver[sizeof(p->from_ver) - 1] = 0;
    return true;
}

static void pend_save(const pend_t *p)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, p, sizeof(*p));
    nvs_commit(h);
    nvs_close(h);
}

static void pend_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY);
    nvs_commit(h);
    nvs_close(h);
}

// ------------------------------------------------------------------ старт и подтверждение

void ota_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    const bool verify = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    ESP_LOGI(TAG, "прошивка %s (%s), слот %s%s", ota_version(), ota_build_date(), run ? run->label : "?",
             verify ? ", ждёт подтверждения" : "");

    pend_t p;
    const bool has = pend_load(&p);
    if (has && run && strcmp(p.label, run->label) != 0) {
        // Записывали в другой слот, а работаем из этого — загрузчик откатил обновление
        ESP_LOGE(TAG, "откат: прошивка %s не подтвердилась", p.ver);
        tg_notify("⚠️ Новая прошивка %s не запустилась нормально — автоматически вернулась версия %s.",
                  p.ver, ota_version());
        pend_clear();
        return;
    }
    if (verify) {
        s_pending = true;
        if (has) {
            s_pend = p;
        } else {
            memset(&s_pend, 0, sizeof(s_pend));
            strlcpy(s_pend.from_ver, "?", sizeof(s_pend.from_ver));
            s_pend.need_wifi = settings()->wifi_ssid[0] != 0;
            s_pend.need_tg = settings()->tg_token[0] != 0;
        }
        return;
    }
    if (has) {
        // Слот новый, но подтверждение не требуется (загрузчик без отката) — просто сообщаем
        tg_notify("✅ Прошивка обновлена: %s → %s", p.from_ver, ota_version());
        pend_clear();
    }
}

void ota_tick(bool bms_ok)
{
    if (!s_pending) return;
    const int64_t up = esp_timer_get_time() / 1000000;
    if (up < OTA_CONFIRM_MIN_S) return;

    const bool bms  = !s_pend.need_bms || bms_ok;
    const bool wifi = !s_pend.need_wifi || net_wait_connected(0);
    const bool tg   = !s_pend.need_tg || tg_online();
    if (bms && wifi && tg) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        s_pending = false;
        pend_clear();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "не удалось подтвердить прошивку: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "прошивка %s подтверждена", ota_version());
        tg_notify("✅ Прошивка обновлена: %s → %s. Всё работает, старая версия больше не нужна.",
                  s_pend.from_ver, ota_version());
        return;
    }
    if (up >= OTA_CONFIRM_TIMEOUT_S) {
        ESP_LOGE(TAG, "прошивка не подтвердилась за %d с (BMS %d, Wi-Fi %d, Telegram %d) — откат",
                 OTA_CONFIRM_TIMEOUT_S, bms, wifi, tg);
        // Запись в NVS остаётся: старая прошивка после отката сообщит об этом в Telegram
        esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "откат невозможен — остаёмся на этой версии");
        s_pending = false;
    }
}

// ------------------------------------------------------------------ запись образа

esp_err_t ota_writer_begin(ota_writer_t *w, size_t total)
{
    memset(w, 0, sizeof(*w));
    if (atomic_exchange(&s_busy, true)) {
        strlcpy(w->err, "Уже идёт обновление", sizeof(w->err));
        return ESP_ERR_INVALID_STATE;
    }
    w->part = esp_ota_get_next_update_partition(NULL);
    if (!w->part) {
        strlcpy(w->err, "Нет слота для обновления (старая разметка флеша?)", sizeof(w->err));
        atomic_store(&s_busy, false);
        return ESP_ERR_NOT_FOUND;
    }
    if (total && (total < OTA_HDR_LEN || total > w->part->size)) {
        snprintf(w->err, sizeof(w->err), "Неверный размер файла: %u КБ (слот %u КБ)",
                 (unsigned)(total / 1024), (unsigned)(w->part->size / 1024));
        atomic_store(&s_busy, false);
        return ESP_ERR_INVALID_SIZE;
    }
    w->total = total;
    ESP_LOGI(TAG, "запись в слот %s, %u байт", w->part->label, (unsigned)total);
    return ESP_OK;
}

static bool check_header(ota_writer_t *w)
{
    const esp_image_header_t *ih = (const esp_image_header_t *)w->hdr;
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC) {
        strlcpy(w->err, "Это не прошивка ESP32: нужен файл build/JkongTelegramm.bin", sizeof(w->err));
        return false;
    }
    if (ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        strlcpy(w->err, "Прошивка собрана для другого чипа", sizeof(w->err));
        return false;
    }
    const esp_app_desc_t *d =
        (const esp_app_desc_t *)(w->hdr + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        strlcpy(w->err, "Это не прошивка приложения (bootloader или таблица разделов?)", sizeof(w->err));
        return false;
    }
    if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof(d->project_name)) != 0) {
        snprintf(w->err, sizeof(w->err), "Прошивка другого проекта: %.31s", d->project_name);
        return false;
    }
    snprintf(w->new_ver, sizeof(w->new_ver), "%.31s", d->version);
    return true;
}

static esp_err_t write_raw(ota_writer_t *w, const void *data, size_t len)
{
    if (w->written + len > w->part->size || (w->total && w->written + len > w->total)) {
        strlcpy(w->err, "Файл больше ожидаемого", sizeof(w->err));
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_ota_write(w->h, data, len);
    if (err != ESP_OK) {
        snprintf(w->err, sizeof(w->err), "Ошибка записи во флеш: %s", esp_err_to_name(err));
        return err;
    }
    w->written += len;
    return ESP_OK;
}

esp_err_t ota_writer_write(ota_writer_t *w, const void *data, size_t len)
{
    const uint8_t *d = data;
    if (!w->opened) {
        // Копим заголовок и проверяем его до того, как стирать слот
        size_t n = OTA_HDR_LEN - w->hdr_len;
        if (n > len) n = len;
        memcpy(w->hdr + w->hdr_len, d, n);
        w->hdr_len += n;
        d += n;
        len -= n;
        if (w->hdr_len < OTA_HDR_LEN) return ESP_OK;
        if (!check_header(w)) return ESP_ERR_INVALID_VERSION;

        esp_err_t err = esp_ota_begin(w->part, OTA_WITH_SEQUENTIAL_WRITES, &w->h);
        if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            strlcpy(w->err, "Текущая прошивка ещё не подтвердила себя — подождите до 5 минут", sizeof(w->err));
            return err;
        }
        if (err != ESP_OK) {
            snprintf(w->err, sizeof(w->err), "esp_ota_begin: %s", esp_err_to_name(err));
            return err;
        }
        w->opened = true;
        err = write_raw(w, w->hdr, OTA_HDR_LEN);
        if (err != ESP_OK) return err;
    }
    return len ? write_raw(w, d, len) : ESP_OK;
}

esp_err_t ota_writer_finish(ota_writer_t *w)
{
    if (!w->opened || (w->total && w->written != w->total)) {
        strlcpy(w->err, "Файл получен не полностью", sizeof(w->err));
        ota_writer_abort(w);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_ota_end(w->h);
    w->opened = false;
    if (err != ESP_OK) {
        strlcpy(w->err, err == ESP_ERR_OTA_VALIDATE_FAILED ? "Образ повреждён (не прошёл проверку)"
                                                           : esp_err_to_name(err), sizeof(w->err));
        atomic_store(&s_busy, false);
        return err;
    }
    err = esp_ota_set_boot_partition(w->part);
    if (err != ESP_OK) {
        snprintf(w->err, sizeof(w->err), "Не удалось выбрать новый слот: %s", esp_err_to_name(err));
        atomic_store(&s_busy, false);
        return err;
    }

    app_status_t s;
    control_get_status(&s);
    pend_t p = {
        .need_bms  = s.bms_ok,
        .need_wifi = net_wait_connected(0),
        .need_tg   = tg_online(),
    };
    strlcpy(p.label, w->part->label, sizeof(p.label));
    strlcpy(p.ver, w->new_ver, sizeof(p.ver));
    strlcpy(p.from_ver, ota_version(), sizeof(p.from_ver));
    pend_save(&p);
    ESP_LOGW(TAG, "прошивка %s записана в %s (%u байт), нужна перезагрузка", w->new_ver, w->part->label,
             (unsigned)w->written);
    // s_busy не сбрасываем: дальше только перезагрузка
    return ESP_OK;
}

void ota_writer_abort(ota_writer_t *w)
{
    if (w->opened) esp_ota_abort(w->h);
    w->opened = false;
    atomic_store(&s_busy, false);
}

// ------------------------------------------------------------------ перезагрузка

static void reboot_task(void *arg)
{
    tg_wait_idle(8000);     // дать уйти сообщениям «обновляюсь»
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void ota_reboot_soon(void)
{
    if (xTaskCreate(reboot_task, "reboot", 3072, NULL, 3, NULL) != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}
