#include "net.h"
#include <string.h>
#include <stdio.h>
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "config.h"
#include "settings.h"

static const char *TAG = "net";
static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta;
static volatile bool s_ap_on;
static int64_t s_ap_forced_until_s;     // 0 = не по кнопке
static int64_t s_last_connected_s;
static volatile bool s_connecting;
#define BIT_CONNECTED BIT0

static int64_t now_s(void) { return esp_timer_get_time() / 1000000; }
static bool has_sta(void) { return settings()->wifi_ssid[0] != 0; }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // Переподключением занимается net_task: пока к точке доступа кто-то подключён,
    // попытки STA не делаем — сканирование каналов рвёт связь с телефоном.
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_CONNECTED);
        s_connecting = false;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connecting = false;
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Wi-Fi подключён, IP: " IPSTR " — страница настроек http://" IPSTR,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.ip));
        s_last_connected_s = now_s();
        xEventGroupSetBits(s_ev, BIT_CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "к точке доступа подключился клиент");
    }
}

static void ap_set(bool on)
{
    if (on == s_ap_on) return;
    if (on) {
        wifi_config_t ap = {0};
        strlcpy((char *)ap.ap.ssid, SETUP_AP_SSID, sizeof(ap.ap.ssid));
        ap.ap.ssid_len = strlen(SETUP_AP_SSID);
        strlcpy((char *)ap.ap.password, settings()->web_pass, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.max_connection = 2;
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap);
        ESP_LOGW(TAG, "точка доступа \"%s\" включена: http://192.168.4.1", SETUP_AP_SSID);
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "точка доступа выключена");
    }
    s_ap_on = on;
}

static int ap_clients(void)
{
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}

// Раз в секунду: кнопка BOOT и решение, нужна ли точка доступа
static void net_task(void *arg)
{
    int held = 0;
    int64_t idle_since = 0, last_try = -1000;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const int64_t t = now_s();

        // Переподключение к домашнему Wi-Fi
        if (s_connecting && t - last_try > 60) s_connecting = false;   // подстраховка
        if (has_sta() && !s_connecting && !(xEventGroupGetBits(s_ev) & BIT_CONNECTED) &&
            t - last_try >= (s_ap_on ? 30 : 5) && !(s_ap_on && ap_clients() > 0)) {
            last_try = t;
            s_connecting = true;
            if (esp_wifi_connect() != ESP_OK) s_connecting = false;
        }

        if (gpio_get_level(SETUP_BUTTON_PIN) == 0) {
            if (++held == SETUP_BUTTON_HOLD_S) {
                ESP_LOGW(TAG, "кнопка: включаю точку доступа на %d мин", SETUP_AP_FORCED_MIN);
                s_ap_forced_until_s = t + SETUP_AP_FORCED_MIN * 60;
            }
        } else {
            held = 0;
        }

        const bool connected = xEventGroupGetBits(s_ev) & BIT_CONNECTED;
        const bool forced = s_ap_forced_until_s && t < s_ap_forced_until_s;
        bool want;
        if (!has_sta() || forced) {
            want = true;
        } else if (!connected) {
            want = s_ap_on || t - s_last_connected_s >= SETUP_AP_AFTER_S;
        } else {
            want = s_ap_on;   // подключились — выключим, когда к точке никто не подключён 2 мин
            if (s_ap_on) {
                if (ap_clients() > 0) idle_since = 0;
                else if (!idle_since) idle_since = t;
                else if (t - idle_since >= 120) want = false;
            }
        }
        if (!want) idle_since = 0;
        ap_set(want);
    }
}

void net_start(void)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    const gpio_config_t btn = {
        .pin_bit_mask = 1ULL << SETUP_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   // настройки храним сами
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, settings()->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, settings()->wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = settings()->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_last_connected_s = now_s();

    if (!has_sta()) {
        ESP_LOGW(TAG, "Wi-Fi не настроен");
        ap_set(true);
    }

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    esp_netif_sntp_init(&sntp);

    xTaskCreate(net_task, "net", 3072, NULL, 3, NULL);
}

bool net_wait_connected(TickType_t timeout)
{
    return xEventGroupWaitBits(s_ev, BIT_CONNECTED, pdFALSE, pdTRUE, timeout) & BIT_CONNECTED;
}

void net_get_ip(char *buf, size_t size)
{
    buf[0] = 0;
    esp_netif_ip_info_t ip;
    if (s_sta && (xEventGroupGetBits(s_ev) & BIT_CONNECTED) && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK) {
        snprintf(buf, size, IPSTR, IP2STR(&ip.ip));
    }
}

bool net_ap_active(void) { return s_ap_on; }
