#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// Дописать в конец строки (без переполнения)
static inline void strappend(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static inline void strappend(char *buf, size_t size, const char *fmt, ...)
{
    size_t len = strlen(buf);
    if (len + 1 >= size) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + len, size - len, fmt, ap);
    va_end(ap);
}

// 3725 -> "1 ч 02 мин"
static inline void fmt_duration(int64_t s, char *buf, size_t size)
{
    if (s < 0) s = 0;
    int64_t m = s / 60, h = m / 60, d = h / 24;
    if (d > 0)      snprintf(buf, size, "%d д %d ч", (int)d, (int)(h % 24));
    else if (h > 0) snprintf(buf, size, "%d ч %02d мин", (int)h, (int)(m % 60));
    else            snprintf(buf, size, "%d мин", (int)m);
}
