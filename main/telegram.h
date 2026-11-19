#pragma once
/*
 * Telegram-бот через HTTPS (api.telegram.org):
 *  - задача отправки: очередь сообщений, повтор при отсутствии связи;
 *  - задача приёма: long polling getUpdates.
 * Уведомления уходят всем chat_id из настроек, команды принимаются от любого из них,
 * ответ на команду — только тому, кто спросил.
 * Каждое сообщение начинается с имени устройства — удобно, если два дома пишут в одну группу.
 */
#include <stdint.h>
#include <stdbool.h>

void tg_init(void);                              // очередь (до control_start)
void tg_start(void);                             // задачи (после net_start)
void tg_send(const char *text);                  // всем получателям
void tg_send_to(int64_t chat_id, const char *text);
void tg_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool tg_online(void);                            // бот хотя бы раз ответил (getMe)
void tg_wait_idle(int timeout_ms);               // подождать, пока очередь отправки опустеет
