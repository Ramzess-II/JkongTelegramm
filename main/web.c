#include "web.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "config.h"
#include "settings.h"
#include "control.h"
#include "net.h"
#include "util.h"
#include "ota.h"
#include "telegram.h"
#include "diag.h"

static const char *TAG = "web";

#define FORM_MAX 2048

// ------------------------------------------------------------------ helpers

static void sendf(httpd_req_t *req, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sendf(httpd_req_t *req, const char *fmt, ...)
{
    char buf[1024];   // длинные блоки с кириллицей не влезали в 512 и обрезались
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    httpd_resp_sendstr_chunk(req, buf);
}

// Экранирование для value="..."
static const char *esc(const char *s, char *out, size_t size)
{
    size_t o = 0;
    for (; *s && o + 7 < size; s++) {
        const char *r = NULL;
        switch (*s) {
        case '&': r = "&amp;"; break;
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '"': r = "&quot;"; break;
        case '\'': r = "&#39;"; break;
        }
        if (r) { strcpy(out + o, r); o += strlen(r); }
        else out[o++] = *s;
    }
    out[o] = 0;
    return out;
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

static bool form_get(const char *body, const char *key, char *out, size_t size)
{
    out[0] = 0;
    if (httpd_query_key_value(body, key, out, size) != ESP_OK) return false;
    url_decode(out);
    // обрезаем пробелы по краям
    char *s = out;
    while (isspace((unsigned char)*s)) s++;
    memmove(out, s, strlen(s) + 1);
    for (int i = (int)strlen(out) - 1; i >= 0 && isspace((unsigned char)out[i]); i--) out[i] = 0;
    return true;
}

static bool form_num(const char *body, const char *key, float *out)
{
    char v[24];
    if (!form_get(body, key, v, sizeof(v)) || !v[0]) return false;
    for (char *p = v; *p; p++) if (*p == ',') *p = '.';
    char *end;
    float f = strtof(v, &end);
    if (end == v) return false;
    *out = f;
    return true;
}

static bool check_auth(httpd_req_t *req)
{
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[120];
        size_t len = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &len, (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[len] = 0;
            char *colon = strchr((char *)dec, ':');
            if (colon && strcmp(colon + 1, settings()->web_pass) == 0) return true;
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"JK BMS\", charset=\"UTF-8\"");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "Нужен пароль");
    return false;
}

static void restart_cb(void *arg) { esp_restart(); }

static void schedule_restart(void)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = { .callback = restart_cb, .name = "restart" };
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 1500 * 1000);
}

static const char *CSS =
    "<style>body{font-family:system-ui,sans-serif;max-width:640px;margin:0 auto;padding:12px 16px;"
    "background:#f4f5f7;color:#1d2125}h1{font-size:22px}fieldset{background:#fff;border:1px solid #d6d9de;"
    "border-radius:10px;margin:0 0 14px;padding:10px 14px}legend{font-weight:600;padding:0 6px}"
    "label{display:block;margin:8px 0 2px;font-size:14px;color:#44494f}input{width:100%;box-sizing:border-box;"
    "padding:8px;font-size:16px;border:1px solid #c4c8ce;border-radius:6px}input[type=checkbox]{width:auto}"
    ".row{display:flex;gap:10px}.row>div{flex:1}.t{font-size:13px;color:#0b6e4f}.h{font-size:12px;color:#6b7178}"
    "button{padding:12px 18px;font-size:16px;border:0;border-radius:8px;background:#1f6feb;color:#fff;width:100%}"
    ".sec{background:#6b7178;margin-top:8px}.st{background:#fff;border-radius:10px;padding:10px 14px;"
    "border:1px solid #d6d9de;margin-bottom:14px;line-height:1.6}"
    "@media(prefers-color-scheme:dark){body{background:#16181c;color:#e6e8eb}fieldset,.st{background:#1f2227;"
    "border-color:#33373d}input{background:#16181c;color:#e6e8eb;border-color:#3b4048}label{color:#b4b9bf}"
    ".t{color:#4cc38a}}</style>";

static void page_head(httpd_req_t *req, const char *title)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    sendf(req, "<!doctype html><html lang=ru><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'><title>%s</title>", title);
    httpd_resp_sendstr_chunk(req, CSS);
    httpd_resp_sendstr_chunk(req, "</head><body>");
}

static void input_text(httpd_req_t *req, const char *label, const char *name, const char *val, const char *type,
                       const char *hint)
{
    char e[200];
    sendf(req, "<label>%s</label><input name=%s type=%s value=\"%s\">", label, name, type, esc(val, e, sizeof(e)));
    if (hint) sendf(req, "<div class=h>%s</div>", hint);
}

static void input_cellv(httpd_req_t *req, const char *label, const char *name, uint16_t mv)
{
    sendf(req, "<div><label>%s</label><input class=cv id=%s name=%s inputmode=decimal value=\"%.3f\">"
               "<div class=t id=%s_t></div></div>", label, name, name, mv / 1000.0f, name);
}

// ------------------------------------------------------------------ диагностика BMS

// Блок «Связь с BMS» на главной; данные подтягивает JS из GET /bms раз в 2 с
static void bms_diag_block(httpd_req_t *req)
{
    sendf(req, "<fieldset><legend>Связь с BMS (проверка подключения)</legend>"
               "<div id=bst style='font-weight:600'>Загрузка…</div><div class=h id=bhint></div>"
               "<div class=h style='margin-top:6px'>ESP TX = GPIO%d → RX BMS · ESP RX = GPIO%d ← TX BMS · "
               "GND общий · %d бод</div>"
               "<div id=bcnt class=h style='margin-top:6px'></div>"
               "<div class=h>Последний ответ: <span id=braw style='font-family:monospace;word-break:break-all'></span></div>"
               "<div id=bdata style='margin-top:8px;line-height:1.6'></div></fieldset>",
          BMS_UART_TX_PIN, BMS_UART_RX_PIN, BMS_UART_BAUD);
    sendf(req, "<fieldset><legend>Реле зарядки (проверка)</legend>"
               "<div>Пин GPIO%d: <b id=rpin>…</b> <span id=rinfo class=h></span></div>"
               "<div class=row style='margin-top:8px'>"
               "<div><button type=button onclick='rt(10)'>Вкл на 10 с</button></div>"
               "<div><button type=button onclick='rt(300)'>Вкл на 5 мин</button></div>"
               "<div><button type=button class=sec style='margin-top:0' onclick='rt(0)'>Выключить</button></div></div>"
               "<div class=t id=rmsg></div>"
               "<div class=h>Тест работает и без BMS, затем реле само вернётся к автоматике. Не включается, если сети нет "
               "или батарея заряжена. При включённом заряде ток в блоке выше должен стать положительным.</div></fieldset>",
          RELAY_PIN);
    httpd_resp_sendstr_chunk(req,
        "<script>(function(){var R=["
        "['Опроса ещё не было',''],"
        "['✅ BMS отвечает, данные верные','Пайка UART в порядке.'],"
        "['❌ BMS молчит: в ответ ни одного байта','Проверьте: общий GND; TX/RX накрест (TX ESP → RX BMS, RX ESP ← TX BMS); "
        "BMS включена и не спит (нажмите кнопку активации); провод к нужным контактам разъёма. "
        "Если сомневаетесь — поменяйте TX и RX местами.'],"
        "['⚠️ Приходят байты, но не кадры JK','Похоже на неверную скорость, помехи или отсутствие общего GND. Проверьте, что "
        "это UART-разъём BMS и уровень сигнала 3.3 В (при 5 В нужен делитель).'],"
        "['⚠️ Кадр от BMS битый или обрывается','Связь есть, но данные искажаются: плохой контакт, длинные или "
        "незакрученные провода, помехи. Если повторяется постоянно — включите BMS_DEBUG_DUMP и пришлите лог.'],"
        "['⚠️ ESP получает собственный запрос','Линии TX и RX замкнуты между собой (перемычка, капля припоя) или "
        "оба провода сидят на одном контакте BMS.']];"
        "function $(i){return document.getElementById(i)}"
        "function f(n,d){return n.toFixed(d)}"
        "function up(){fetch('/bms',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){"
        "var r=R[j.res]||R[0];var st=r[0];"
        "if(j.res!=1&&j.ever)st+=' (последний успешный ответ '+j.ago+' с назад)';"
        "$('bst').textContent=st;$('bst').style.color=j.res==1?'#1a7f37':j.res==0?'':'#cf222e';"
        "$('bhint').textContent=r[1];"
        "$('bcnt').textContent='Опросов: '+j.polls+' · успешно: '+j.ok+' · без ответа: '+j.nr+' · мусор: '+j.gb+"
        "' · битых: '+j.bf+(j.ec?' · эхо: '+j.ec:'')+' · байт в последнем: '+j.n;"
        "$('braw').textContent=j.raw||'—';"
        "$('rpin').textContent=j.rp?'ВКЛ (высокий уровень)':'ВЫКЛ';$('rpin').style.color=j.rp?'#1a7f37':'';"
        "$('rinfo').textContent=j.rt>0?'· тест, осталось '+j.rt+' с':j.rl?'· включено автоматикой':'';"
        "if(!j.ever){$('bdata').innerHTML='';return}"
        "var h='<b>'+f(j.v,2)+' В</b> · SOC '+j.soc+'% · '+(j.i>0?'+':'')+f(j.i,2)+' А';"
        "h+='<br>Температуры: MOS '+j.t[0]+' °C, T1 '+j.t[1]+' °C, T2 '+j.t[2]+' °C';"
        "h+='<br>Ёмкость в BMS: '+(j.cap?j.cap+' Ач':'нет')+' · циклов: '+j.cyc+' · протокол: '+(j.proto<0?'?':j.proto)+"
        "(j.al?' · <b style=\"color:#cf222e\">тревоги: 0x'+j.al.toString(16)+'</b>':'');"
        "if(j.c.length){h+='<br>Ячеек: '+j.c.length+' · Δ '+j.d+' мВ<div style=\"display:grid;grid-template-columns:"
        "repeat(auto-fill,minmax(88px,1fr));gap:4px;margin-top:4px\">';"
        "j.c.forEach(function(v,k){var c=(k+1==j.cmax)?'#cf222e':(k+1==j.cmin)?'#1f6feb':'';"
        "h+='<div style=\"border:1px solid #8884;border-radius:6px;padding:2px 6px;font-size:14px'+(c?';color:'+c:'')+"
        "'\">'+(k+1)+': '+f(v/1000,3)+'</div>'});h+='</div>'}"
        "$('bdata').innerHTML=h;"
        "}).catch(function(){$('bst').textContent='Нет связи с устройством';$('bst').style.color='#cf222e'})}"
        "window.rt=function(s){var m=$('rmsg');m.textContent='…';"
        "fetch('/relay',{method:'POST',body:'s='+s}).then(function(r){return r.text()})"
        ".then(function(t){m.textContent=t;setTimeout(up,1200)}).catch(function(){m.textContent='Нет связи с устройством'})};"
        "up();setInterval(up,2000)})()</script>");
}

static esp_err_t h_bms(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    app_status_t s;
    control_get_status(&s);
    jk_bms_diag_t g;
    jk_bms_get_diag(&g);

    const size_t size = 1600;
    char *b = malloc(size);
    if (!b) return ESP_FAIL;
    b[0] = 0;

    char raw[JK_DIAG_RAW * 3 + 4] = "";
    for (int i = 0; i < g.raw_len; i++) strappend(raw, sizeof(raw), "%s%02X", i ? " " : "", g.raw[i]);
    if (g.last_bytes > g.raw_len) strappend(raw, sizeof(raw), " …");

    strappend(b, size, "{\"res\":%d,\"polls\":%lu,\"ok\":%lu,\"nr\":%lu,\"gb\":%lu,\"bf\":%lu,\"ec\":%lu,"
                       "\"n\":%u,\"raw\":\"%s\",\"ever\":%s,\"ago\":%lld,\"rl\":%d,\"rp\":%d,\"rt\":%d",
              (int)g.last, (unsigned long)g.polls, (unsigned long)g.ok, (unsigned long)g.no_reply,
              (unsigned long)g.garbage, (unsigned long)g.bad_frame, (unsigned long)g.echo,
              g.last_bytes, raw, s.bms_ever ? "true" : "false",
              s.bms_ever ? (long long)(s.now_s - s.bms_last_ok_s) : -1LL,
              s.relay_on, s.relay_pin, s.relay_test_left_s);
    if (s.bms_ever) {
        const jk_bms_data_t *d = &s.bms;
        strappend(b, size, ",\"v\":%.3f,\"i\":%.2f,\"soc\":%u,\"t\":[%d,%d,%d],\"cap\":%lu,\"cyc\":%u,"
                           "\"proto\":%d,\"al\":%u,\"cmin\":%u,\"cmax\":%u,\"d\":%u,\"c\":[",
                  d->voltage_mv / 1000.0, d->current_ma / 1000.0, d->soc, d->temp_mos, d->temp_t1, d->temp_t2,
                  (unsigned long)d->capacity_ah, d->cycles, d->has_protocol ? d->protocol : -1, d->alarms,
                  d->cell_min_num, d->cell_max_num,
                  d->cell_count ? d->cell_max_mv - d->cell_min_mv : 0);
        for (int i = 0; i < d->cell_count && i < JK_MAX_CELLS; i++)
            strappend(b, size, "%s%u", i ? "," : "", d->cell_mv[i]);
        strappend(b, size, "]");
    }
    strappend(b, size, ",\"up\":%lld,\"rr\":%d}", (long long)(esp_timer_get_time() / 1000000), (int)diag_reset_reason());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, b);
    free(b);
    return ESP_OK;
}

// GET /log: причина перезагрузки, лог прошлого запуска (с дампом паники) и текущий лог
static esp_err_t h_log(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char up[32];
    fmt_duration(esp_timer_get_time() / 1000000, up, sizeof(up));
    sendf(req, "Прошивка %s · работает %s (%lld с)\nПричина последней перезагрузки: %s\n"
               "Свободно памяти: %u байт (минимум было %u)\n\n",
          ota_version(), up, (long long)(esp_timer_get_time() / 1000000), diag_reset_reason_str(),
          (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    const char *prev = diag_prev_log();
    httpd_resp_sendstr_chunk(req, "===== Лог прошлого запуска (конец, вместе с дампом паники) =====\n");
    httpd_resp_sendstr_chunk(req, prev[0] ? prev : "(нет — было включение питания или лог пуст)\n");
    httpd_resp_sendstr_chunk(req, "\n\n===== Текущий запуск =====\n");
    char *cur = diag_cur_log_dup();
    httpd_resp_sendstr_chunk(req, cur ? cur : "(нет памяти)");
    free(cur);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static int read_body(httpd_req_t *req, char *buf, size_t size);

// POST /relay, тело "s=<секунд>": тестовое включение реле, 0 — выключить
static esp_err_t h_relay(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    char body[32];
    float sec = 0;
    if (read_body(req, body, sizeof(body)) < 0 || !form_num(body, "s", &sec)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "Неверный запрос");
        return ESP_OK;
    }
    const int s = (int)sec;
    const char *err = control_relay_test(s);
    char msg[160];
    if (err)        snprintf(msg, sizeof(msg), "⚠️ %s", err);
    else if (s > 0) snprintf(msg, sizeof(msg), "✅ Реле включено на %d с", s > 600 ? 600 : s);
    else            snprintf(msg, sizeof(msg), "Тест остановлен, реле вернулось к автоматике");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

// ------------------------------------------------------------------ GET /

static esp_err_t h_index(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    const settings_t *c = settings();
    app_status_t s;
    control_get_status(&s);
    char ip[20], tmp[64];
    net_get_ip(ip, sizeof(ip));

    page_head(req, "JK BMS");
    sendf(req, "<h1>🔋 %s</h1><div class=st>", esc(c->device_name, tmp, sizeof(tmp)));
    if (s.bms_ok) {
        sendf(req, "%.2f В · %u%% · %+.1f А<br>", s.bms.voltage_mv / 1000.0f, s.bms.soc, s.bms.current_ma / 1000.0f);
        if (s.bms.cell_count)
            sendf(req, "Ячейки %.3f…%.3f В (Δ %u мВ)<br>", s.bms.cell_min_mv / 1000.0f, s.bms.cell_max_mv / 1000.0f,
                  s.bms.cell_max_mv - s.bms.cell_min_mv);
    } else {
        sendf(req, "⚠️ BMS не отвечает<br>");
    }
    sendf(req, "Сеть: %s · Заряд: %s<br>Wi-Fi: %s<br>",
          s.grid == GRID_ON ? "есть" : s.grid == GRID_OFF ? "<b>нет</b>" : "определяется",
          s.relay_on ? control_reason_str(s.reason) : "выкл", ip[0] ? ip : "не подключён");
    char up[32];
    fmt_duration(esp_timer_get_time() / 1000000, up, sizeof(up));
    sendf(req, "Работает %s · прошлая перезагрузка: %s%s%s · <a href=/log>лог</a></div>", up,
          diag_reset_abnormal() ? "<b style='color:#cf222e'>" : "", diag_reset_reason_str(),
          diag_reset_abnormal() ? "</b>" : "");

    bms_diag_block(req);

    httpd_resp_sendstr_chunk(req, "<form method=post action=/save>");

    httpd_resp_sendstr_chunk(req, "<fieldset><legend>Связь</legend>");
    input_text(req, "Название (в начале каждого сообщения)", "name", c->device_name, "text", NULL);
    input_text(req, "Wi-Fi: имя сети", "ssid", c->wifi_ssid, "text", NULL);
    input_text(req, "Wi-Fi: пароль", "wpass", "", "password", c->wifi_pass[0] ? "пусто — оставить как есть" : NULL);
    input_text(req, "Токен бота (@BotFather)", "token", "", "password",
               c->tg_token[0] ? "задан; пусто — оставить как есть" : "не задан");
    char chats[100] = "";
    for (int i = 0; i < SET_MAX_CHATS; i++) {
        if (c->chat_ids[i]) strappend(chats, sizeof(chats), "%s%lld", chats[0] ? ", " : "", (long long)c->chat_ids[i]);
    }
    input_text(req, "Получатели: chat_id через запятую (до 4)", "chats", chats, "text",
               "ваш, жены, id группы. Свой chat_id бот пришлёт, если ему написать");
    input_text(req, "Пароль этой страницы и точки доступа", "webpass", "", "password", "пусто — оставить; минимум 8 символов");
    httpd_resp_sendstr_chunk(req, "</fieldset>");

    httpd_resp_sendstr_chunk(req, "<fieldset><legend>Батарея</legend><div class=row>");
    sendf(req, "<div><label>Ячеек (S)</label><input id=cells name=cells inputmode=numeric value=%u></div>", c->cells);
    sendf(req, "<div><label>Ёмкость, Ач (0 = из BMS)</label><input name=cap inputmode=numeric value=%u></div>", c->capacity_ah);
    sendf(req, "<div><label>Мин. SOC, %%</label><input name=minsoc inputmode=numeric value=%u></div>", c->min_soc);
    httpd_resp_sendstr_chunk(req, "</div><div class=h>Мин. SOC — ниже этого энергия не считается доступной (отсечка инвертора).</div></fieldset>");

    httpd_resp_sendstr_chunk(req, "<fieldset><legend>Пороги заряда, В на ячейку</legend><div class=row>");
    input_cellv(req, "Выключить заряд", "off", c->cell_off_mv);
    input_cellv(req, "Включить заряд", "on", c->cell_on_mv);
    httpd_resp_sendstr_chunk(req, "</div><div class=row>");
    input_cellv(req, "Аварийно включить", "fon", c->cell_force_on_mv);
    input_cellv(req, "Аварийный — минимум до", "foff", c->cell_force_off_mv);
    httpd_resp_sendstr_chunk(req, "</div><div class=h>После выключения заряд снова включится, только когда напряжение "
                                  "упадёт до порога «включить». Ночью заряд включается ниже этого же порога.</div></fieldset>");

    httpd_resp_sendstr_chunk(req, "<fieldset><legend>Ночной тариф и резерв</legend><div class=row>");
    sendf(req, "<div><label>Ночь с, ч</label><input name=ns inputmode=numeric value=%u></div>", c->night_start);
    sendf(req, "<div><label>до, ч</label><input name=ne inputmode=numeric value=%u></div>", c->night_end);
    httpd_resp_sendstr_chunk(req, "</div><div class=h>Одинаковые часы — ночного тарифа нет.</div><div class=row>");
    sendf(req, "<div><label>Резерв, часов</label><input name=rh inputmode=numeric value=%u></div>", c->reserve_h);
    sendf(req, "<div><label>Запас сверху, %%</label><input name=rhyst inputmode=numeric value=%u></div>", c->reserve_hyst_pct);
    sendf(req, "<div><label>Нагрузка по умолч., Вт</label><input name=dload inputmode=numeric value=%u></div>", c->default_load_w);
    httpd_resp_sendstr_chunk(req, "</div><div class=h>Днём заряд включается, если энергии меньше, чем нужно на резерв по "
                                  "выученному профилю потребления. Нагрузка по умолчанию — пока профиль пустой.</div></fieldset>");

    httpd_resp_sendstr_chunk(req, "<fieldset><legend>Определение пропажи сети</legend><div class=row>");
    sendf(req, "<div><label>Сеть пропала: разряд больше, А</label><input name=glost inputmode=decimal value=%.1f></div>", c->grid_lost_a);
    sendf(req, "<div><label>Сеть вернулась: разряд меньше, А</label><input name=gback inputmode=decimal value=%.1f></div>", c->grid_back_a);
    sendf(req, "</div><label><input type=checkbox name=csign value=1%s> Инвертировать знак тока BMS</label></fieldset>",
          c->current_sign < 0 ? " checked" : "");

    httpd_resp_sendstr_chunk(req, "<button>Сохранить и перезагрузить</button></form>"
                                  "<form method=post action=/reset onsubmit=\"return confirm('Сбросить пороги и резерв к значениям "
                                  "по умолчанию? Wi-Fi, бот и получатели сохранятся.')\">"
                                  "<button class=sec>Сбросить пороги по умолчанию</button></form>");

    char ver[32];
    sendf(req, "<fieldset style='margin-top:14px'><legend>Обновление прошивки</legend>"
               "<div class=h>Сейчас: версия %s от %s. Файл: build/JkongTelegramm.bin</div>",
          esc(ota_version(), ver, sizeof(ver)), ota_build_date());
    httpd_resp_sendstr_chunk(req,
        "<label>Файл прошивки</label><input type=file id=fw accept='.bin'>"
        "<button type=button class=sec onclick='fwUp()'>Загрузить прошивку</button>"
        "<div class=t id=fwst></div><div class=h>Если новая прошивка не заработает, через 5 минут "
        "автоматически вернётся старая.</div></fieldset>"
        "<script>function fwUp(){var f=document.getElementById('fw').files[0],s=document.getElementById('fwst');"
        "if(!f){s.textContent='Выберите файл .bin';return}if(!confirm('Прошить '+f.name+'?'))return;"
        "var x=new XMLHttpRequest();x.open('POST','/ota');"
        "x.upload.onprogress=function(e){if(e.lengthComputable)s.textContent='Загрузка '+Math.round(e.loaded*100/e.total)+'%'};"
        "x.onload=function(){s.textContent=x.responseText;if(x.status==200)setTimeout(function(){location.reload()},25000)};"
        "x.onerror=function(){s.textContent='Ошибка связи'};s.textContent='Загрузка…';x.send(f)}</script>");
    httpd_resp_sendstr_chunk(req,
        "<script>function u(){var n=parseFloat(document.getElementById('cells').value)||0;"
        "document.querySelectorAll('.cv').forEach(function(e){var v=parseFloat(e.value.replace(',','.'));"
        "document.getElementById(e.id+'_t').textContent=isNaN(v)?'':'= '+(v*n).toFixed(2)+' В на пакет'})}"
        "document.querySelectorAll('input').forEach(function(e){e.addEventListener('input',u)});u()</script>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ------------------------------------------------------------------ POST /save, /reset

static void result_page(httpd_req_t *req, bool ok, const char *msg)
{
    page_head(req, ok ? "Сохранено" : "Ошибка");
    char e[200];
    if (ok) {
        sendf(req, "<h1>✅ %s</h1><p>Устройство перезагружается. Если менялся Wi-Fi — ищите новый IP "
                   "в Telegram (/settings).</p><meta http-equiv=refresh content='10;url=/'>", esc(msg, e, sizeof(e)));
    } else {
        sendf(req, "<h1>⚠️ Не сохранено</h1><p>%s</p><p><a href='javascript:history.back()'>← Назад</a></p>",
              esc(msg, e, sizeof(e)));
    }
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
}

static int read_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len >= size) return -1;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) return -1;
        got += r;
    }
    buf[got] = 0;
    return got;
}

static esp_err_t h_save(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    char *body = malloc(FORM_MAX);
    if (!body) return ESP_FAIL;
    if (read_body(req, body, FORM_MAX) < 0) {
        free(body);
        result_page(req, false, "Слишком большой запрос");
        return ESP_OK;
    }

    settings_t n = *settings();
    char v[100];
    float f;

    if (form_get(body, "name", v, sizeof(v)) && v[0]) strlcpy(n.device_name, v, sizeof(n.device_name));
    if (form_get(body, "ssid", v, sizeof(v))) strlcpy(n.wifi_ssid, v, sizeof(n.wifi_ssid));
    if (form_get(body, "wpass", v, sizeof(v)) && v[0]) strlcpy(n.wifi_pass, v, sizeof(n.wifi_pass));
    if (form_get(body, "token", v, sizeof(v)) && v[0]) strlcpy(n.tg_token, v, sizeof(n.tg_token));
    if (form_get(body, "chats", v, sizeof(v))) settings_parse_chats(v, n.chat_ids);
    if (form_get(body, "webpass", v, sizeof(v)) && v[0]) strlcpy(n.web_pass, v, sizeof(n.web_pass));

    if (form_num(body, "cells", &f))  n.cells = (f >= 1 && f <= 32) ? (uint8_t)f : 0;
    if (form_num(body, "cap", &f))    n.capacity_ah = f < 0 ? 0 : (uint16_t)f;
    if (form_num(body, "minsoc", &f)) n.min_soc = f < 0 ? 0 : (uint8_t)f;
    if (form_num(body, "off", &f))    n.cell_off_mv = (uint16_t)lroundf(f * 1000);
    if (form_num(body, "on", &f))     n.cell_on_mv = (uint16_t)lroundf(f * 1000);
    if (form_num(body, "fon", &f))    n.cell_force_on_mv = (uint16_t)lroundf(f * 1000);
    if (form_num(body, "foff", &f))   n.cell_force_off_mv = (uint16_t)lroundf(f * 1000);
    if (form_num(body, "ns", &f))     n.night_start = (uint8_t)f;
    if (form_num(body, "ne", &f))     n.night_end = (uint8_t)f;
    if (form_num(body, "rh", &f))     n.reserve_h = f < 0 ? 0 : (uint8_t)f;
    if (form_num(body, "rhyst", &f))  n.reserve_hyst_pct = f < 0 ? 0 : (uint8_t)f;
    if (form_num(body, "dload", &f))  n.default_load_w = f < 0 ? 0 : (uint16_t)f;
    if (form_num(body, "glost", &f))  n.grid_lost_a = f;
    if (form_num(body, "gback", &f))  n.grid_back_a = f;
    n.current_sign = form_get(body, "csign", v, sizeof(v)) ? -1 : 1;
    free(body);

    const char *err = settings_validate(&n);
    if (err) {
        result_page(req, false, err);
        return ESP_OK;
    }
    if (settings_save(&n) != ESP_OK) {
        result_page(req, false, "Ошибка записи во флеш");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "настройки сохранены, перезагрузка");
    result_page(req, true, "Настройки сохранены");
    schedule_restart();
    return ESP_OK;
}

static esp_err_t h_reset(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    settings_t n = *settings();
    settings_defaults(&n, true);
    if (settings_save(&n) != ESP_OK) {
        result_page(req, false, "Ошибка записи во флеш");
        return ESP_OK;
    }
    result_page(req, true, "Пороги сброшены по умолчанию");
    schedule_restart();
    return ESP_OK;
}

// ------------------------------------------------------------------ POST /ota (тело = файл .bin как есть)

static esp_err_t ota_reply(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t h_ota(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    ota_writer_t *w = malloc(sizeof(*w));
    char *buf = malloc(4096);
    if (!w || !buf) {
        free(w);
        free(buf);
        return ota_reply(req, "500 Internal Server Error", "Нет памяти");
    }
    char msg[256];
    esp_err_t err = ota_writer_begin(w, req->content_len);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "обновление через веб: %u байт", (unsigned)req->content_len);
        size_t left = req->content_len;
        int timeouts = 0;
        while (left > 0 && err == ESP_OK) {
            int r = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
            if (r <= 0) {
                strlcpy(w->err, "Связь прервалась во время загрузки", sizeof(w->err));
                err = ESP_FAIL;
                break;
            }
            timeouts = 0;
            left -= r;
            err = ota_writer_write(w, buf, r);
        }
        if (err == ESP_OK) err = ota_writer_finish(w);
        else ota_writer_abort(w);
    }
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "обновление не удалось: %s", w->err);
        snprintf(msg, sizeof(msg), "⚠️ Не удалось: %s", w->err);
        free(w);
        return ota_reply(req, "400 Bad Request", msg);
    }
    snprintf(msg, sizeof(msg), "✅ Прошивка %s записана. Перезагрузка, страница обновится сама через ~25 с.", w->new_ver);
    tg_notify("🔄 Обновляю прошивку через веб-страницу: %s → %s, перезагружаюсь…", ota_version(), w->new_ver);
    free(w);
    ota_reply(req, "200 OK", msg);
    ota_reboot_soon();
    return ESP_OK;
}

void web_start(void)
{
    _Static_assert(CONFIG_LWIP_MAX_SOCKETS >= 5 + 3 + 5, "LWIP_MAX_SOCKETS: веб-серверу 5+3, Telegram и прочим нужно ещё ~5");
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;      // при нехватке закрываем самое старое соединение
    // Браузер держит по нескольку соединений. По умолчанию 7 (+2 служебных) — при лимите lwIP 10
    // Telegram не мог открыть сокет. Оставляем веб-серверу 5, остальное — Telegram, DNS, SNTP.
    cfg.max_open_sockets = 5;
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "не удалось запустить веб-сервер");
        return;
    }
    const httpd_uri_t idx  = { .uri = "/",      .method = HTTP_GET,  .handler = h_index };
    const httpd_uri_t save = { .uri = "/save",  .method = HTTP_POST, .handler = h_save };
    const httpd_uri_t rst  = { .uri = "/reset", .method = HTTP_POST, .handler = h_reset };
    httpd_register_uri_handler(srv, &idx);
    httpd_register_uri_handler(srv, &save);
    httpd_register_uri_handler(srv, &rst);
    const httpd_uri_t ota  = { .uri = "/ota",   .method = HTTP_POST, .handler = h_ota };
    httpd_register_uri_handler(srv, &ota);
    const httpd_uri_t bms  = { .uri = "/bms",   .method = HTTP_GET,  .handler = h_bms };
    httpd_register_uri_handler(srv, &bms);
    const httpd_uri_t rel  = { .uri = "/relay", .method = HTTP_POST, .handler = h_relay };
    httpd_register_uri_handler(srv, &rel);
    const httpd_uri_t lg   = { .uri = "/log",   .method = HTTP_GET,  .handler = h_log };
    httpd_register_uri_handler(srv, &lg);
}
