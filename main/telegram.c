#include "telegram.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"
#include "config.h"
#include "settings.h"
#include "net.h"
#include "commands.h"
#include "ota.h"
#include "diag.h"
#include "util.h"

static const char *TAG = "telegram";

#define TG_QUEUE_LEN      32
#define TG_POLL_TIMEOUT_S 25
#define TG_POLL_BUF       8192
#define TG_SEND_BUF       1024
#define TG_REPLY_BUF      3072
#define TG_LATE_S         120        // старше — отвечаем с пометкой «пришла с опозданием»
#define TG_MAX_AGE_S      (30 * 60)  // старше — не выполняем (но отвечаем, что не выполнили)

// Задачи бота сбрасывают watchdog между ожиданиями до 10 с и long poll до (TG_POLL_TIMEOUT_S + 10) с.
// С таймаутом 5 с (значение ESP-IDF по умолчанию; 90 из sdkconfig.defaults ESP-IDF 5.5 молча отбрасывал,
// максимум 60) плата перезагружалась через 6.5 с после старта. Не даём собрать такую прошивку.
_Static_assert(CONFIG_ESP_TASK_WDT_TIMEOUT_S >= TG_POLL_TIMEOUT_S + 20,
               "Task WDT too short for Telegram tasks: set CONFIG_ESP_TASK_WDT_TIMEOUT_S=60 (max in ESP-IDF 5.5)");

typedef struct {
    int64_t chat;
    char   *text;
    int64_t created_us;   // esp_timer при постановке в очередь — для «случилось в …» при опоздании
} tg_msg_t;

#define TG_DELIVERY_LATE_S 60   // пролежало в очереди дольше — дописываем время события

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} resp_t;

static QueueHandle_t s_q;
static char s_username[64];
static volatile bool s_reboot_after_ack;   // прошивка записана: подтвердить offset и перезагрузиться

bool tg_online(void) { return s_username[0] != 0; }

void tg_wait_idle(int timeout_ms)
{
    if (!s_q) return;
    for (int t = 0; t < timeout_ms && uxQueueMessagesWaiting(s_q) > 0; t += 100) vTaskDelay(pdMS_TO_TICKS(100));
}

// ------------------------------------------------------------------ HTTP

// ------------------------------------------------------------------ Время сервера Telegram
//
// Каждый ответ Bot API несёт заголовок Date. Это те же часы, по которым Telegram ставит поле date
// в сообщениях, поэтому возраст команды по ним верен и сразу после перезагрузки, ещё до NTP.

static portMUX_TYPE s_time_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t      s_srv_epoch;      // время сервера из последнего Date, 0 — ещё не было
static int64_t      s_srv_mono_us;    // esp_timer в момент получения

// 1970-01-01 → дни для григорианской даты (H. Hinnant, days_from_civil)
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = (int)(y - era * 400);
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// "Wed, 19 Nov 2026 10:15:42 GMT" → секунды Unix; 0 — не разобрали
static int64_t parse_http_date(const char *s)
{
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char m3[4] = "";
    int d, y, hh, mm, ss;
    if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &d, m3, &y, &hh, &mm, &ss) != 6) return 0;
    const char *p = strstr(mon, m3);
    if (!p || strlen(m3) != 3 || (p - mon) % 3) return 0;
    const int m = (int)(p - mon) / 3 + 1;
    if (y < 2024 || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 60) return 0;
    return days_from_civil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
}

static void on_server_date(const char *value)
{
    const int64_t t = parse_http_date(value);
    if (!t) return;
    portENTER_CRITICAL(&s_time_mux);
    const bool first = s_srv_epoch == 0;
    s_srv_epoch = t;
    s_srv_mono_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_time_mux);

    // NTP ещё не успел — ставим системное время по Telegram (ночной тариф, время в сообщениях).
    // Когда придёт NTP, он поправит его сам.
    if (time(NULL) < 1700000000) {
        struct timeval tv = { .tv_sec = (time_t)t };
        settimeofday(&tv, NULL);
        ESP_LOGW(TAG, "время взято из ответа Telegram (NTP ещё нет)");
    } else if (first) {
        ESP_LOGI(TAG, "часы Telegram: расхождение с системными %lld с", (long long)(time(NULL) - t));
    }
}

// Текущее время по часам Telegram, иначе системное (если синхронизировано). false — неизвестно.
static bool tg_now(time_t *out)
{
    portENTER_CRITICAL(&s_time_mux);
    const int64_t e = s_srv_epoch, mono = s_srv_mono_us;
    portEXIT_CRITICAL(&s_time_mux);
    if (e) {
        *out = (time_t)(e + (esp_timer_get_time() - mono) / 1000000);
        return true;
    }
    const time_t now = time(NULL);
    if (now < 1700000000) return false;
    *out = now;
    return true;
}

static esp_err_t http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
        strcasecmp(e->header_key, "Date") == 0) {
        on_server_date(e->header_value);
    } else if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        resp_t *r = e->user_data;
        size_t n = e->data_len;
        if (r->len + n >= r->cap) n = r->cap - 1 - r->len;
        memcpy(r->buf + r->len, e->data, n);
        r->len += n;
        r->buf[r->len] = 0;
    }
    return ESP_OK;
}

// Вызов метода Bot API. Возвращает HTTP-статус или -1 при сетевой ошибке.
static int tg_call(esp_http_client_handle_t *pc, resp_t *r, int timeout_ms,
                   const char *method, const char *query, const char *json_body)
{
    if (!*pc) {
        esp_http_client_config_t cfg = {
            .url = "https://api.telegram.org/",
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = timeout_ms,
            .event_handler = http_event,
            .user_data = r,
            .keep_alive_enable = true,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
        };
        *pc = esp_http_client_init(&cfg);
        if (!*pc) return -1;
    }
    esp_http_client_handle_t c = *pc;

    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s%s", settings()->tg_token, method, query ? query : "");
    esp_http_client_set_url(c, url);
    if (json_body) {
        esp_http_client_set_method(c, HTTP_METHOD_POST);
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, json_body, strlen(json_body));
    } else {
        esp_http_client_set_method(c, HTTP_METHOD_GET);
        esp_http_client_set_post_field(c, NULL, 0);
    }

    r->len = 0;
    r->buf[0] = 0;
    esp_err_t err = esp_http_client_perform(c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", method, esp_err_to_name(err));
        esp_http_client_cleanup(c);   // соединение пересоздадим
        *pc = NULL;
        return -1;
    }
    return esp_http_client_get_status_code(c);
}

// ------------------------------------------------------------------ Отправка

void tg_send_to(int64_t chat_id, const char *text)
{
    if (!s_q || !text || chat_id == 0 || !settings()->tg_token[0]) return;
    const char *name = settings()->device_name;
    size_t n = strlen(name) + strlen(text) + 8;
    tg_msg_t m = { .chat = chat_id, .text = malloc(n), .created_us = esp_timer_get_time() };
    if (!m.text) return;
    snprintf(m.text, n, "🏠 %s\n%s", name, text);
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "очередь полна, сообщение потеряно: %s", text);
        free(m.text);
    }
}

void tg_send(const char *text)
{
    for (int i = 0; i < SET_MAX_CHATS; i++) tg_send_to(settings()->chat_ids[i], text);
}

void tg_notify(const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tg_send(buf);
}

// Сообщение пролежало в очереди (не было интернета)? Тогда «🕒 Случилось в 14:32, доставлено с опозданием 3 ч 05 мин».
// Задержка считается по монотонному таймеру, поэтому верна, даже если в момент события время было неизвестно.
static void delivery_note(const tg_msg_t *m, char *out, size_t size)
{
    out[0] = 0;
    const int64_t delay_s = (esp_timer_get_time() - m->created_us) / 1000000;
    if (delay_s < TG_DELIVERY_LATE_S) return;

    char dur[32];
    fmt_duration(delay_s, dur, sizeof(dur));
    time_t now;
    if (!tg_now(&now)) {
        snprintf(out, size, "\n\n🕒 Доставлено с опозданием %s (не было связи)", dur);
        return;
    }
    const time_t ev = now - (time_t)delay_s;
    struct tm te, tn;
    localtime_r(&ev, &te);
    localtime_r(&now, &tn);
    char when[24];
    if (te.tm_yday == tn.tm_yday && te.tm_year == tn.tm_year) strftime(when, sizeof(when), "%H:%M", &te);
    else                                                       strftime(when, sizeof(when), "%d.%m %H:%M", &te);
    snprintf(out, size, "\n\n🕒 Случилось в %s, доставлено с опозданием %s (не было связи)", when, dur);
}

static char *build_send_body(const tg_msg_t *m)
{
    char chat[24], note[128];
    snprintf(chat, sizeof(chat), "%lld", (long long)m->chat);
    delivery_note(m, note, sizeof(note));

    char *text = m->text;
    if (note[0]) {
        const size_t n = strlen(m->text) + strlen(note) + 1;
        text = malloc(n);
        if (!text) return NULL;
        snprintf(text, n, "%s%s", m->text, note);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "chat_id", chat);
    cJSON_AddStringToObject(o, "text", text);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (text != m->text) free(text);
    return body;
}

static void send_task(void *arg)
{
    esp_task_wdt_add(NULL);
    static char rbuf[TG_SEND_BUF];
    resp_t r = { rbuf, sizeof(rbuf), 0 };
    esp_http_client_handle_t c = NULL;

    for (;;) {
        esp_task_wdt_reset();
        tg_msg_t m;
        if (xQueuePeek(s_q, &m, pdMS_TO_TICKS(10000)) != pdTRUE) continue;
        if (!net_wait_connected(pdMS_TO_TICKS(10000))) continue;

        char *body = build_send_body(&m);
        int st = body ? tg_call(&c, &r, 15000, "sendMessage", NULL, body) : -1;
        free(body);

        if (st == 200 || (st >= 400 && st < 500 && st != 429)) {
            // 4xx (кроме 429) повтором не исправить — выкидываем
            if (st != 200) ESP_LOGE(TAG, "sendMessage %d (chat %lld): %s", st, (long long)m.chat, rbuf);
            xQueueReceive(s_q, &m, 0);
            free(m.text);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

// ------------------------------------------------------------------ Приём

// Сколько секунд назад отправлено сообщение (по часам Telegram); -1 — неизвестно
static long msg_age_s(cJSON *date)
{
    time_t now;
    if (!cJSON_IsNumber(date) || !tg_now(&now)) return -1;
    long age = (long)(now - (time_t)date->valuedouble);
    return age < 0 ? 0 : age;
}

// Команда пришла с опозданием (например, роутер перезагружался): выполняем, если не старше
// TG_MAX_AGE_S, и пишем об опоздании. Старее — не выполняем, но отвечаем, чтобы не было тишины.
static void handle_text(int64_t chat_id, const char *text, long age_s)
{
    if (text[0] != '/') return;

    char cmd[32];
    size_t i = 0;
    const char *p = text;
    while (*p && *p != ' ' && *p != '\n' && i < sizeof(cmd) - 1) cmd[i++] = *p++;
    cmd[i] = 0;
    while (*p == ' ') p++;

    // /status@other_bot в общей группе — не нам
    char *at = strchr(cmd, '@');
    if (at) {
        *at = 0;
        if (s_username[0] && strcasecmp(at + 1, s_username) != 0) return;
    }

    char *reply = calloc(1, TG_REPLY_BUF);
    if (!reply) return;
    if (age_s > TG_MAX_AGE_S) {
        ESP_LOGW(TAG, "команда %s пришла через %ld с — не выполняю", cmd, age_s);
        snprintf(reply, TG_REPLY_BUF, "⏭ Команда %s пришла через %ld мин после отправки (не было связи) и не выполнена: "
                                      "слишком старая. Отправьте её снова.", cmd, age_s / 60);
    } else {
        size_t off = 0;
        if (age_s > TG_LATE_S) {
            snprintf(reply, TG_REPLY_BUF, "⏱ Команда пришла с опозданием %ld мин (не было связи).\n", (age_s + 30) / 60);
            off = strlen(reply);
        }
        commands_handle(cmd, p, reply + off, TG_REPLY_BUF - off);
        if (!reply[off]) reply[0] = 0;
    }
    if (reply[0]) tg_send_to(chat_id, reply);
    free(reply);
}

static void answer_stranger(int64_t chat_id)
{
    // Подсказываем chat_id, чтобы его можно было добавить в настройки. Не чаще раза в 10 с.
    static int64_t last_us;
    int64_t now = esp_timer_get_time();
    if (last_us && now - last_us < 10000000) return;
    last_us = now;
    char buf[160];
    snprintf(buf, sizeof(buf), "⛔ Нет доступа. Ваш chat_id: %lld — попросите добавить его на веб-странице устройства.",
             (long long)chat_id);
    tg_send_to(chat_id, buf);
}

// ------------------------------------------------------------------ Прошивка файлом

static bool ends_with_bin(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".bin") == 0;
}

// Файл адресован этому боту? В личке — да. В группе (там может быть бот второго дома) —
// только ответом на сообщение этого бота или с @именем бота в подписи.
static bool doc_for_me(cJSON *m)
{
    cJSON *chat = cJSON_GetObjectItem(m, "chat");
    cJSON *type = chat ? cJSON_GetObjectItem(chat, "type") : NULL;
    if (cJSON_IsString(type) && strcmp(type->valuestring, "private") == 0) return true;
    if (!s_username[0]) return false;

    cJSON *rep = cJSON_GetObjectItem(m, "reply_to_message");
    cJSON *from = rep ? cJSON_GetObjectItem(rep, "from") : NULL;
    cJSON *un = from ? cJSON_GetObjectItem(from, "username") : NULL;
    if (cJSON_IsString(un) && strcasecmp(un->valuestring, s_username) == 0) return true;

    cJSON *cap = cJSON_GetObjectItem(m, "caption");
    if (cJSON_IsString(cap)) {
        char at[70];
        snprintf(at, sizeof(at), "@%s", s_username);
        size_t al = strlen(at);
        for (const char *p = cap->valuestring; *p; p++) {
            if (strncasecmp(p, at, al) == 0) return true;
        }
    }
    return false;
}

// Скачать файл с серверов Telegram прямо в слот OTA
static bool download_to_ota(const char *file_path, size_t size, ota_writer_t *w)
{
    char *url = malloc(512);
    char *buf = malloc(4096);
    esp_http_client_handle_t c = NULL;
    bool ok = false;
    if (!url || !buf) {
        strlcpy(w->err, "Нет памяти", sizeof(w->err));
        goto out;
    }
    snprintf(url, 512, "https://api.telegram.org/file/bot%s/%s", settings()->tg_token, file_path);
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };
    c = esp_http_client_init(&cfg);
    if (!c || esp_http_client_open(c, 0) != ESP_OK) {
        strlcpy(w->err, "Не удалось скачать файл", sizeof(w->err));
        goto out;
    }
    int64_t len = esp_http_client_fetch_headers(c);
    int st = esp_http_client_get_status_code(c);
    if (st != 200) {
        snprintf(w->err, sizeof(w->err), "Скачивание: HTTP %d", st);
        goto out;
    }
    if (len > 0 && (size_t)len != size) w->total = (size_t)len;
    for (;;) {
        esp_task_wdt_reset();
        int r = esp_http_client_read(c, buf, 4096);
        if (r < 0) {
            strlcpy(w->err, "Связь прервалась во время скачивания", sizeof(w->err));
            goto out;
        }
        if (r == 0) break;
        if (ota_writer_write(w, buf, r) != ESP_OK) goto out;
    }
    ok = true;
out:
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(url);
    free(buf);
    return ok;
}

static void handle_document(esp_http_client_handle_t *pc, resp_t *r, int64_t chat_id, cJSON *doc)
{
    cJSON *name = cJSON_GetObjectItem(doc, "file_name");
    cJSON *fid = cJSON_GetObjectItem(doc, "file_id");
    cJSON *fsz = cJSON_GetObjectItem(doc, "file_size");
    if (!cJSON_IsString(name) || !cJSON_IsString(fid) || !ends_with_bin(name->valuestring)) return;

    char msg[384];
    const size_t size = cJSON_IsNumber(fsz) ? (size_t)fsz->valuedouble : 0;
    if (size > 20 * 1024 * 1024) {
        tg_send_to(chat_id, "⚠️ Файл больше 20 МБ — бот не может его скачать");
        return;
    }

    ota_writer_t *w = malloc(sizeof(*w));
    if (!w) return;
    if (ota_writer_begin(w, size) != ESP_OK) {
        snprintf(msg, sizeof(msg), "⚠️ Обновление невозможно: %s", w->err);
        tg_send_to(chat_id, msg);
        free(w);
        return;
    }
    snprintf(msg, sizeof(msg), "⬇️ Скачиваю %s (%u КБ)…", name->valuestring, (unsigned)(size / 1024));
    tg_send_to(chat_id, msg);

    // getFile → путь к файлу на сервере
    char q[200];
    snprintf(q, sizeof(q), "?file_id=%s", fid->valuestring);
    char path[128] = "";
    if (tg_call(pc, r, 15000, "getFile", q, NULL) == 200) {
        cJSON *root = cJSON_Parse(r->buf);
        cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
        cJSON *fp = res ? cJSON_GetObjectItem(res, "file_path") : NULL;
        if (cJSON_IsString(fp)) strlcpy(path, fp->valuestring, sizeof(path));
        cJSON_Delete(root);
    }
    bool ok = false;
    if (!path[0]) strlcpy(w->err, "Telegram не отдал путь к файлу (getFile)", sizeof(w->err));
    else ok = download_to_ota(path, size, w);

    if (ok && ota_writer_finish(w) == ESP_OK) {
        ESP_LOGW(TAG, "прошивка %s получена через Telegram", w->new_ver);
        snprintf(msg, sizeof(msg), "🔄 Обновляю прошивку: %s → %s, перезагружаюсь. "
                                   "Если новая версия не заработает, через 5 минут вернётся старая.",
                 ota_version(), w->new_ver);
        tg_send(msg);
        s_reboot_after_ack = true;
    } else {
        if (!ok) ota_writer_abort(w);   // при ошибке finish слот уже освобождён
        ESP_LOGE(TAG, "обновление через Telegram не удалось: %s", w->err);
        snprintf(msg, sizeof(msg), "⚠️ Прошивка не обновлена: %s", w->err);
        tg_send_to(chat_id, msg);
    }
    free(w);
}

static long long max_update_id_raw(const char *buf)
{
    long long best = -1;
    for (const char *p = buf; (p = strstr(p, "\"update_id\":")) != NULL; p++) {
        long long v = strtoll(p + 12, NULL, 10);
        if (v > best) best = v;
    }
    return best;
}

static void handle_updates(esp_http_client_handle_t *pc, resp_t *rr, const char *buf, long long *offset)
{
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        // Ответ обрезан или битый — пропускаем эти апдейты, чтобы не зациклиться
        long long id = max_update_id_raw(buf);
        if (id >= 0) *offset = id + 1;
        ESP_LOGW(TAG, "не удалось разобрать getUpdates");
        return;
    }
    cJSON *u;
    cJSON_ArrayForEach(u, cJSON_GetObjectItem(root, "result")) {
        // Прошивка уже записана: остальные апдейты не подтверждаем — получим их после перезагрузки
        if (s_reboot_after_ack) break;
        cJSON *uid = cJSON_GetObjectItem(u, "update_id");
        if (cJSON_IsNumber(uid)) *offset = (long long)uid->valuedouble + 1;

        cJSON *m = cJSON_GetObjectItem(u, "message");
        cJSON *chat = m ? cJSON_GetObjectItem(m, "chat") : NULL;
        cJSON *cid = chat ? cJSON_GetObjectItem(chat, "id") : NULL;
        cJSON *txt = m ? cJSON_GetObjectItem(m, "text") : NULL;
        cJSON *date = m ? cJSON_GetObjectItem(m, "date") : NULL;
        cJSON *doc = m ? cJSON_GetObjectItem(m, "document") : NULL;
        if (!cJSON_IsNumber(cid) || (!cJSON_IsString(txt) && !cJSON_IsObject(doc))) continue;

        const char *what = cJSON_IsString(txt) ? txt->valuestring : "[файл]";
        int64_t chat_id = (int64_t)cid->valuedouble;
        if (!settings_chat_allowed(chat_id)) {
            ESP_LOGW(TAG, "сообщение из чужого чата %lld: %s", (long long)chat_id, what);
            answer_stranger(chat_id);
            continue;
        }
        const long age = msg_age_s(date);
        // Если обработка уронит плату — после перезагрузки эта команда будет пропущена
        diag_inflight_set(cJSON_IsNumber(uid) ? (long long)uid->valuedouble : -1, what);
        if (cJSON_IsObject(doc)) {
            if (!doc_for_me(m)) {
                // не нам (файл в группе для бота второго дома)
            } else if (age > TG_MAX_AGE_S) {
                ESP_LOGW(TAG, "файл пришёл через %ld с — не прошиваю", age);
                tg_send_to(chat_id, "⏭ Файл прошивки пришёл слишком поздно (не было связи) и не установлен. "
                                    "Пришлите его снова.");
            } else {
                handle_document(pc, rr, chat_id, doc);
            }
        } else {
            ESP_LOGI(TAG, "команда от %lld: %s (отправлена %ld с назад)", (long long)chat_id, txt->valuestring, age);
            handle_text(chat_id, txt->valuestring, age);
        }
        diag_inflight_clear();
    }
    cJSON_Delete(root);
}

static void poll_task(void *arg)
{
    esp_task_wdt_add(NULL);
    char *buf = malloc(TG_POLL_BUF);
    resp_t r = { buf, TG_POLL_BUF, 0 };
    esp_http_client_handle_t c = NULL;
    long long offset = 0;
    const int timeout_ms = (TG_POLL_TIMEOUT_S + 10) * 1000;

    // Узнаём имя бота, чтобы различать /cmd@bot в группе
    while (!s_username[0]) {
        esp_task_wdt_reset();
        if (!net_wait_connected(pdMS_TO_TICKS(10000))) continue;
        int st = tg_call(&c, &r, timeout_ms, "getMe", NULL, NULL);
        if (st == 200) {
            cJSON *root = cJSON_Parse(buf);
            cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
            cJSON *un = res ? cJSON_GetObjectItem(res, "username") : NULL;
            if (cJSON_IsString(un)) strlcpy(s_username, un->valuestring, sizeof(s_username));
            cJSON_Delete(root);
            ESP_LOGI(TAG, "бот @%s на связи", s_username);
        } else {
            ESP_LOGW(TAG, "getMe: %d %s", st, st > 0 ? buf : "");
            if (st == 401 || st == 404) ESP_LOGE(TAG, "неверный токен бота — проверьте на веб-странице");
            vTaskDelay(pdMS_TO_TICKS(st == 401 || st == 404 ? 60000 : 10000));
        }
    }

    // Прошлый запуск упал на обработке команды: пропускаем её, иначе Telegram отдаст её снова
    char crash_cmd[64];
    int64_t crash_id;
    if (diag_inflight_crashed(&crash_id, crash_cmd, sizeof(crash_cmd)) && crash_id >= 0) {
        offset = crash_id + 1;
        ESP_LOGE(TAG, "команда «%s» (update %lld) уронила плату — пропускаю", crash_cmd, (long long)crash_id);
        char msg[320];
        snprintf(msg, sizeof(msg), "⚠️ Команда «%s» вызвала сбой прошивки (%s) и пропущена. Подробности: /log на веб-странице.",
                 crash_cmd, diag_reset_reason_str());
        tg_send(msg);
    }

    char q[128];
    for (;;) {
        esp_task_wdt_reset();
        if (!net_wait_connected(pdMS_TO_TICKS(10000))) continue;
        snprintf(q, sizeof(q), "?timeout=%d&limit=5&offset=%lld&allowed_updates=%%5B%%22message%%22%%5D",
                 TG_POLL_TIMEOUT_S, offset);
        int st = tg_call(&c, &r, timeout_ms, "getUpdates", q, NULL);
        esp_task_wdt_reset();
        if (st == 200) {
            // handle_updates может сам вызывать API (getFile) — разбираем копию ответа
            char *copy = strdup(buf);
            if (copy) {
                handle_updates(&c, &r, copy, &offset);
                free(copy);
            } else {
                long long id = max_update_id_raw(buf);
                if (id >= 0) offset = id + 1;
            }
            if (s_reboot_after_ack) {
                // Подтверждаем Telegram, что апдейт с файлом обработан, иначе после перезагрузки
                // он придёт снова и прошивка запишется по кругу
                snprintf(q, sizeof(q), "?timeout=0&limit=1&offset=%lld", offset);
                for (int i = 0; i < 3 && tg_call(&c, &r, 15000, "getUpdates", q, NULL) != 200; i++) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                ota_reboot_soon();
                for (;;) {
                    esp_task_wdt_reset();
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        } else {
            if (st == 409) ESP_LOGE(TAG, "этот токен уже опрашивает другое устройство/программа (409)");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

void tg_init(void)
{
    s_q = xQueueCreate(TG_QUEUE_LEN, sizeof(tg_msg_t));
}

void tg_start(void)
{
    if (!settings()->tg_token[0]) {
        ESP_LOGW(TAG, "токен бота не задан — Telegram выключен, настройте на веб-странице");
        return;
    }
    xTaskCreate(send_task, "tg_send", 8192, NULL, 4, NULL);
    xTaskCreate(poll_task, "tg_poll", 8192, NULL, 4, NULL);
}
