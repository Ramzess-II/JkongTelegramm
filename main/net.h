#pragma once
/*
 * Wi-Fi клиент с автопереподключением + SNTP.
 * Точка доступа SETUP_AP_SSID (пароль = пароль веб-страницы) включается, если:
 *  - Wi-Fi не настроен;
 *  - к Wi-Fi не удаётся подключиться дольше SETUP_AP_AFTER_S;
 *  - кнопку BOOT удерживали SETUP_BUTTON_HOLD_S секунд (на SETUP_AP_FORCED_MIN минут).
 * Страница настроек в точке доступа: http://192.168.4.1
 */
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"

void net_start(void);
bool net_wait_connected(TickType_t timeout);
void net_get_ip(char *buf, size_t size);    // IP в домашней сети или "" если не подключены
bool net_ap_active(void);
