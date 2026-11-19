#include <stdlib.h>
#include <time.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "config.h"
#include "settings.h"
#include "energy.h"
#include "control.h"
#include "telegram.h"
#include "net.h"
#include "web.h"
#include "ota.h"
#include "diag.h"

void app_main(void)
{
    diag_init();        // лог в RTC-память и причина прошлой перезагрузки — первым делом
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    setenv("TZ", TIMEZONE, 1);
    tzset();

    settings_init();    // настройки из NVS (или по умолчанию)
    energy_init();      // профиль потребления
    tg_init();          // очередь сообщений нужна до запуска управления
    ota_init();         // состояние прошивки после обновления / отката
    control_start();    // реле в OFF, опрос BMS
    net_start();        // Wi-Fi, точка доступа при необходимости, SNTP
    web_start();        // страница настроек
    tg_start();         // бот
}
