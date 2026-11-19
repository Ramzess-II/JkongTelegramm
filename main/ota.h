#pragma once
/*
 * Обновление прошивки по воздуху (OTA) + откат.
 *
 * Источники прошивки: веб-страница (POST /ota) и файл .bin, присланный боту.
 * Оба пишут образ через ota_writer_*: проверяется чип, проект и целостность образа.
 *
 * Откат (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): после перезагрузки новая прошивка «пробная».
 * Она подтверждает себя (ota_tick), когда проработала OTA_CONFIRM_MIN_S и снова работает всё,
 * что работало до обновления: BMS, Wi-Fi, Telegram. Если за OTA_CONFIRM_TIMEOUT_S этого не случилось,
 * или прошивка зависла/упала (watchdog → перезагрузка), загрузчик возвращает предыдущую версию,
 * а бот сообщает об откате.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_ota_ops.h"

#define OTA_CONFIRM_MIN_S      30
#define OTA_CONFIRM_TIMEOUT_S  300
#define OTA_HDR_LEN            288   // заголовок образа + сегмента + esp_app_desc_t

typedef struct {
    esp_ota_handle_t        h;
    const esp_partition_t  *part;
    size_t                  total;      // ожидаемый размер (0 — неизвестен)
    size_t                  written;
    uint8_t                 hdr[OTA_HDR_LEN];
    size_t                  hdr_len;
    bool                    opened;
    char                    new_ver[32];
    char                    err[96];    // причина ошибки по-русски
} ota_writer_t;

void ota_init(void);                         // при старте: состояние прошивки, сообщение об откате
void ota_tick(bool bms_ok);                  // раз в секунду из задачи управления: подтверждение/откат
const char *ota_version(void);               // "1.1.0"
const char *ota_build_date(void);            // "Nov 17 2026"

esp_err_t ota_writer_begin(ota_writer_t *w, size_t total);
esp_err_t ota_writer_write(ota_writer_t *w, const void *data, size_t len);
esp_err_t ota_writer_finish(ota_writer_t *w);   // проверка образа, загрузка с нового слота при перезагрузке
void      ota_writer_abort(ota_writer_t *w);

void ota_reboot_soon(void);                  // перезагрузка после отправки сообщений из очереди (≤ 8 с)
