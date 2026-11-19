#pragma once
/*
 * Диагностика без USB: лог и дамп паники в RTC-памяти, которая переживает перезагрузку.
 *
 * - Обычный лог перехватывается через esp_log_set_vprintf().
 * - Дамп паники (регистры, Backtrace) перехватывается линкером: --wrap=uart_hal_write_txfifo
 *   (через эту функцию обработчик паники пишет в консольный UART0).
 * - После перезагрузки лог прошлого запуска доступен на /log и коротко уходит в Telegram.
 *
 * Плюс «команда в обработке»: если команда Telegram уронила плату, после перезагрузки
 * она пропускается, иначе Telegram отдаёт её снова и плата перезагружается по кругу.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_system.h"

void        diag_init(void);                  // самым первым в app_main
esp_reset_reason_t diag_reset_reason(void);
const char *diag_reset_reason_str(void);
bool        diag_reset_abnormal(void);        // паника, watchdog, просадка питания

const char *diag_prev_log(void);              // лог прошлого запуска ("" если нет)
char       *diag_cur_log_dup(void);           // копия текущего лога (free())
bool        diag_prev_backtrace(char *out, size_t size);   // строка "Backtrace: ..." из прошлого лога

void diag_inflight_set(int64_t update_id, const char *text);
void diag_inflight_clear(void);
// Команда, при обработке которой плата перезагрузилась. true — есть такая.
bool diag_inflight_crashed(int64_t *update_id, char *text, size_t size);
