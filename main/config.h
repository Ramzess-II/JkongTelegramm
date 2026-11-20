#pragma once
/*
 * Значения ПО УМОЛЧАНИЮ (DEF_*) — используются при первой загрузке и после сброса.
 * Дальше они меняются на веб-странице устройства и хранятся во флеше (NVS).
 * Остальное (пины, тайминги) — только здесь, при сборке.
 * Батарея: 13S NMC. Напряжения порогов задаются НА ЯЧЕЙКУ.
 */

// ============================ Значения по умолчанию =================
#define DEF_DEVICE_NAME          "Дом 1"
#define DEF_WEB_PASSWORD         "jkbms1234"  // пароль веб-страницы и точки доступа (мин. 8 символов)

#define DEF_CELLS                13
#define DEF_CAPACITY_AH          0           // ёмкость, Ач. 0 = взять из настроек BMS
#define DEF_MIN_SOC              10          // %, ниже этого энергию не считаем доступной (отсечка инвертора)

#define DEF_CELL_OFF_mV          4050        // выключить заряд (13S: 52.65 В)
#define DEF_CELL_ON_mV           3900        // включить снова / ночью (13S: 50.70 В)
#define DEF_CELL_FORCE_ON_mV     3600        // аварийный заряд в любое время (13S: 46.80 В)
#define DEF_CELL_FORCE_OFF_mV    3700        // аварийный заряд держится минимум до (13S: 48.10 В)

#define DEF_NIGHT_START_HOUR     23          // начало = конец -> ночного тарифа нет
#define DEF_NIGHT_END_HOUR       7

#define DEF_RESERVE_HOURS        4           // сколько часов отключения батарея должна выдержать всегда
#define DEF_RESERVE_HYST_PCT     15          // днём дозаряжаем до резерва + 15 %
#define DEF_LOAD_W               300         // нагрузка, пока профиль не набран

// Сеть считается пропавшей, если батарея отдаёт ток (дом перешёл на инвертор).
#define DEF_GRID_LOST_A          1.0f        // ток разряда выше -> работаем от инвертора
#define DEF_GRID_BACK_A          0.5f        // ток разряда ниже -> сеть вернулась
#define DEF_CURRENT_SIGN         1           // -1, если при заряде ток показывается отрицательным

// ============================ Пины (ESP32-S3 Pico) ==================
#define BMS_UART_NUM             UART_NUM_1
#define BMS_UART_TX_PIN          17          // TX ESP -> RX BMS
#define BMS_UART_RX_PIN          18          // RX ESP <- TX BMS
#define BMS_UART_BAUD            115200
#define RELAY_PIN                7           // реле зарядного устройства
#define RELAY_ACTIVE_LEVEL       1           // 1 = реле включается высоким уровнем
#define SETUP_BUTTON_PIN         0           // кнопка BOOT: удерживать 3 с -> точка доступа
#define SETUP_BUTTON_HOLD_S      3

// ============================ Wi-Fi / веб ===========================
#define SETUP_AP_SSID            "JK-BMS-Setup"
#define SETUP_AP_AFTER_S         180         // нет Wi-Fi столько секунд -> поднять точку доступа
#define SETUP_AP_FORCED_MIN      30          // точка по кнопке живёт столько минут

// ============================ Прочие тайминги =======================
#define PROFILE_EMA_ALPHA        0.3f        // вес нового часа в среднем (0..1)
#define PROFILE_MIN_COVERAGE_S   600         // час учитывается, если от батареи работали хотя бы 10 мин
#define GRID_LOST_DELAY_S        10
#define GRID_BACK_DELAY_S        30
#define BMS_POLL_MS              1000
#define BMS_RESPONSE_TIMEOUT_MS  500
#define BMS_TIMEOUT_S            15          // нет ответа дольше -> реле OFF + сообщение
#define BMS_DEBUG_DUMP           0           // 1 = печатать сырые кадры BMS в лог
#define RELAY_MIN_SWITCH_S       60          // не переключать реле чаще (кроме аварийного выключения)

// ============================ Уведомления ===========================
#define NOTIFY_RELAY             1           // сообщать о включении/выключении заряда

// Тревоги BMS в Telegram (биты поля 0x8B, имена — jk_bms_alarm_name())
// Бит 2 «перенапряжение при заряде» — штатная работа BMS: самая высокая ячейка дошла до порога, BMS
// сама отсекает заряд, тревога мигает при каждом отсечении. Не шлём (видно в /status).
#define ALARM_SILENT_MASK        (1u << 2)
#define ALARM_REPEAT_S           3600        // одну и ту же тревогу — не чаще раза в час
#define ALARM_CLEAR_S            60          // «снята» — только если пропала на минуту
#define WARN_RUNTIME_H           2.0f        // при отключении: предупредить, если запаса меньше N часов
#define WARN_SOC                 20          // при отключении: предупредить, если SOC ниже
#define MANUAL_DEFAULT_H         3           // срок действия /charge_on и /charge_off без аргумента

// ============================ Время =================================
#define TIMEZONE                 "EET-2EEST,M3.5.0/3,M10.5.0/4"   // Киев
#define NTP_SERVER               "pool.ntp.org"
