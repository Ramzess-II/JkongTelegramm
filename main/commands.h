#pragma once
#include <stddef.h>

// cmd — команда без "@bot" ("/status"), args — остаток строки. Ответ пишется в out.
void commands_handle(const char *cmd, const char *args, char *out, size_t size);
