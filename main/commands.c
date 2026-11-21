#include "commands.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "control.h"
#include "energy.h"
#include "util.h"
#include "settings.h"
#include "net.h"
#include "ota.h"

static void cmd_help(char *o, size_t n)
{
    strappend(o, n,
              "Команды:\n"
              "/status — состояние батареи, сети и заряда\n"
              "/cells — напряжения ячеек\n"
              "/profile — потребление дома по часам и прогноз\n"
              "/charge_on [ч] — включить заряд вручную (по умолч. %d ч)\n"
              "/charge_off [ч] — запретить заряд (по умолч. %d ч)\n"
              "/auto — вернуть автоматический режим\n"
              "/settings — текущие настройки и адрес веб-страницы\n"
              "/profile_reset — сбросить выученный профиль\n\n"
              "Обновить прошивку: пришлите боту файл JkongTelegramm.bin "
              "(в группе — ответом на сообщение этого бота)",
              MANUAL_DEFAULT_H, MANUAL_DEFAULT_H);
}

static void cmd_status(char *o, size_t n)
{
    app_status_t s;
    control_get_status(&s);
    char d1[32], d2[32];

    fmt_duration(s.now_s - s.grid_since_s, d1, sizeof(d1));
    switch (s.grid) {
    case GRID_ON:  strappend(o, n, "🔌 Сеть есть (%s)\n", d1); break;
    case GRID_OFF: strappend(o, n, "⚡ Сети НЕТ, от батареи уже %s\n", d1); break;
    default:       strappend(o, n, "❔ Сеть: определяется…\n"); break;
    }

    if (!s.bms_ever) {
        strappend(o, n, "⚠️ BMS ещё ни разу не ответила\n🔁 Заряд: выкл — без данных BMS не включается");
        if (s.mode == MODE_MANUAL_ON) strappend(o, n, " (ручной режим тоже ждёт BMS)");
        return;
    }
    if (!s.bms_ok) {
        fmt_duration(s.now_s - s.bms_last_ok_s, d1, sizeof(d1));
        strappend(o, n, "⚠️ BMS не отвечает %s, ниже последние данные\n", d1);
    }

    const jk_bms_data_t *b = &s.bms;
    strappend(o, n, "🔋 %.2f В · %u%% · %+.1f А", b->voltage_mv / 1000.0f, b->soc, b->current_ma / 1000.0f);
    if (s.load_w > 1) strappend(o, n, " · %.0f Вт", s.load_w);
    strappend(o, n, "\n");
    if (b->cell_count) {
        strappend(o, n, "📊 Ячейки %.3f…%.3f В (Δ %u мВ)\n",
                  b->cell_min_mv / 1000.0f, b->cell_max_mv / 1000.0f, b->cell_max_mv - b->cell_min_mv);
    }
    strappend(o, n, "🌡 MOS %d° · T1 %d° · T2 %d°\n", b->temp_mos, b->temp_t1, b->temp_t2);

    strappend(o, n, "🔁 Заряд: %s", s.relay_on ? "ВКЛ" : "выкл");
    if (s.relay_on) strappend(o, n, " (%s)", control_reason_str(s.reason));
    if (s.relay_hold) strappend(o, n, "\n📌 Реле включено постоянно (галочка на веб-странице)");
    const char *blk = s.relay_on ? NULL : control_block_reason(&s);
    if (blk && (s.mode == MODE_MANUAL_ON || s.grid != GRID_OFF)) strappend(o, n, "\n⛔ Не включается: %s", blk);
    if (s.mode != MODE_AUTO) {
        fmt_duration(s.mode_until_s - s.now_s, d2, sizeof(d2));
        strappend(o, n, "\n✋ Ручной режим: %s, ещё %s", s.mode == MODE_MANUAL_ON ? "заряжать" : "не заряжать", d2);
    }
    if (s.full_latched && !s.relay_on)
        strappend(o, n, " · заряжена, ждём ниже %.2f В", set_pack_mv(settings()->cell_on_mv) / 1000.0f);
    strappend(o, n, "\n");

    if (s.capacity_ah > 0) {
        fmt_duration((int64_t)(s.runtime_h * 3600), d1, sizeof(d1));
        strappend(o, n, "⏳ Доступно %.2f кВт·ч → хватит ≈ %s\n", s.avail_wh / 1000.0f, d1);
        strappend(o, n, "🛡 Резерв на %u ч: нужно %.2f кВт·ч — %s\n", settings()->reserve_h, s.reserve_wh / 1000.0f,
                  s.avail_wh >= s.reserve_wh ? "ок" : "МАЛО");
        if (s.grid == GRID_OFF && s.load_w > 1) {
            fmt_duration((int64_t)(s.avail_wh / s.load_w * 3600), d1, sizeof(d1));
            strappend(o, n, "   при текущей нагрузке — ≈ %s\n", d1);
        }
    } else {
        strappend(o, n, "⏳ Ёмкость неизвестна: укажите её на веб-странице устройства\n");
    }
    if (s.grid == GRID_OFF) {
        strappend(o, n, "📉 За это отключение: %.2f кВт·ч\n", s.outage_wh / 1000.0f);
    }

    const uint16_t loud = b->alarms & ~ALARM_SILENT_MASK, quiet = b->alarms & ALARM_SILENT_MASK;
    if (loud) {
        strappend(o, n, "🚨 Тревоги:");
        for (int i = 0; i < 16; i++) {
            if (loud & (1u << i)) strappend(o, n, " %s;", jk_bms_alarm_name(i));
        }
        strappend(o, n, "\n");
    }
    if (quiet) {
        strappend(o, n, "ℹ️ BMS:");
        for (int i = 0; i < 16; i++) {
            if (quiet & (1u << i)) strappend(o, n, " %s;", jk_bms_alarm_name(i));
        }
        strappend(o, n, " (BMS сама отсекает заряд по ячейке)\n");
    }
    if (!s.time_valid) strappend(o, n, "🕒 Время не синхронизировано — ночной тариф не учитывается\n");
}

static void cmd_settings(char *o, size_t n)
{
    const settings_t *c = settings();
    char ip[20];
    net_get_ip(ip, sizeof(ip));
    strappend(o, n, "⚙️ Настройки\n");
    strappend(o, n, "Батарея: %uS, ёмкость %s", c->cells, c->capacity_ah ? "" : "из BMS");
    if (c->capacity_ah) strappend(o, n, "%u Ач", c->capacity_ah);
    strappend(o, n, ", мин. SOC %u%%\n", c->min_soc);
    strappend(o, n, "Выкл заряд: %.3f В/яч (%.2f В)\n", c->cell_off_mv / 1000.0f, set_pack_mv(c->cell_off_mv) / 1000.0f);
    strappend(o, n, "Вкл заряд: %.3f В/яч (%.2f В)\n", c->cell_on_mv / 1000.0f, set_pack_mv(c->cell_on_mv) / 1000.0f);
    strappend(o, n, "Аварийный: %.3f → %.3f В/яч\n", c->cell_force_on_mv / 1000.0f, c->cell_force_off_mv / 1000.0f);
    if (c->night_start != c->night_end) strappend(o, n, "Ночной тариф: %02u:00–%02u:00\n", c->night_start, c->night_end);
    else                                strappend(o, n, "Ночной тариф: нет\n");
    strappend(o, n, "Резерв: %u ч (+%u%%)\n", c->reserve_h, c->reserve_hyst_pct);
    strappend(o, n, "Сеть пропала при разряде > %.1f А, вернулась < %.1f А\n", c->grid_lost_a, c->grid_back_a);
    int chats = 0;
    for (int i = 0; i < SET_MAX_CHATS; i++) chats += c->chat_ids[i] != 0;
    strappend(o, n, "Получателей: %d\n", chats);
    strappend(o, n, "Прошивка: %s от %s\n", ota_version(), ota_build_date());
    if (ip[0]) strappend(o, n, "Изменить: http://%s", ip);
}

static void cmd_cells(char *o, size_t n)
{
    app_status_t s;
    control_get_status(&s);
    const jk_bms_data_t *b = &s.bms;
    if (!s.bms_ever || !b->cell_count) {
        strappend(o, n, "Нет данных о ячейках");
        return;
    }
    strappend(o, n, "📊 Ячейки (Δ %u мВ):\n", b->cell_max_mv - b->cell_min_mv);
    for (int i = 0; i < b->cell_count; i++) {
        const char *mark = (i + 1 == b->cell_max_num) ? " ⬆" : (i + 1 == b->cell_min_num) ? " ⬇" : "";
        strappend(o, n, "%2d: %.3f В%s\n", i + 1, b->cell_mv[i] / 1000.0f, mark);
    }
}

static void cmd_profile(char *o, size_t n)
{
    app_status_t s;
    control_get_status(&s);
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    const int hour = s.time_valid ? lt.tm_hour : -1;

    strappend(o, n, "📈 Потребление по часам (учится во время отключений):\n");
    for (int h = 0; h < 24; h++) {
        if (energy_hour_known(h)) strappend(o, n, "%02d: %4.0f Вт%s\n", h, energy_hour_w(h), h == hour ? "  ←" : "");
        else                      strappend(o, n, "%02d:    —%s\n", h, h == hour ? "  ←" : "");
    }
    strappend(o, n, "Среднее: %.0f Вт%s\n", energy_mean_w(), energy_mean_known() ? "" : " (по умолчанию)");
    strappend(o, n, "Пустые часы считаются по среднему.\n");
    if (s.capacity_ah > 0) {
        char d[32];
        fmt_duration((int64_t)(s.runtime_h * 3600), d, sizeof(d));
        strappend(o, n, "Прогноз от текущего заряда: ≈ %s", d);
    }
}

static int parse_hours(const char *args)
{
    int h = atoi(args);
    if (h <= 0) h = MANUAL_DEFAULT_H;
    if (h > 48) h = 48;
    return h;
}

void commands_handle(const char *cmd, const char *args, char *o, size_t n)
{
    o[0] = 0;
    if (!strcmp(cmd, "/start") || !strcmp(cmd, "/help")) {
        cmd_help(o, n);
    } else if (!strcmp(cmd, "/status")) {
        cmd_status(o, n);
    } else if (!strcmp(cmd, "/cells")) {
        cmd_cells(o, n);
    } else if (!strcmp(cmd, "/profile")) {
        cmd_profile(o, n);
    } else if (!strcmp(cmd, "/charge_on")) {
        int h = parse_hours(args);
        control_set_mode(MODE_MANUAL_ON, h);
        app_status_t s;
        control_get_status(&s);
        const char *blk = control_block_reason(&s);
        if (blk) {
            strappend(o, n, "✋ Ручной режим «заряжать» на %d ч установлен, но реле сейчас НЕ включится:\n⛔ %s.\n"
                            "Как только причина уйдёт — заряд включится сам (до %.2f В).", h, blk,
                      set_pack_mv(settings()->cell_off_mv) / 1000.0f);
            if (!s.bms_ok) strappend(o, n, "\nПроверить само реле без BMS можно кнопкой на веб-странице (/settings → ссылка).");
        } else {
            strappend(o, n, "✋ Заряд включён вручную на %d ч (выключится сам на %.2f В).", h,
                      set_pack_mv(settings()->cell_off_mv) / 1000.0f);
        }
    } else if (!strcmp(cmd, "/settings")) {
        cmd_settings(o, n);
    } else if (!strcmp(cmd, "/charge_off")) {
        int h = parse_hours(args);
        control_set_mode(MODE_MANUAL_OFF, h);
        strappend(o, n, "✋ Заряд запрещён на %d ч", h);
    } else if (!strcmp(cmd, "/auto")) {
        control_set_mode(MODE_AUTO, 0);
        strappend(o, n, "🤖 Автоматический режим");
    } else if (!strcmp(cmd, "/profile_reset")) {
        control_reset_profile();
        strappend(o, n, "Профиль потребления сброшен");
    } else {
        strappend(o, n, "Не знаю такой команды. /help — список");
    }

    if (!strcmp(cmd, "/charge_on") || !strcmp(cmd, "/charge_off") || !strcmp(cmd, "/auto")) {
        app_status_t s;
        control_get_status(&s);
        if (s.relay_hold)
            strappend(o, n, "\n\n📌 На веб-странице стоит галочка «Реле всегда включено» — пока она стоит, "
                            "команда не действует. Снять её можно только на веб-странице.");
    }
}
