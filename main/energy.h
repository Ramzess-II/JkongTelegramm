#pragma once
/*
 * Профиль потребления дома по часам суток.
 *
 * Пока сеть есть, дом питается от сети и батарея его потребление не видит.
 * Поэтому профиль учится только во время отключений: каждую секунду
 * накапливаем энергию, отданную батареей, и в конце часа обновляем
 * скользящее среднее (EMA) для этого часа. Профиль хранится в NVS
 * и переживает перезагрузку.
 *
 * hour = -1 означает "время неизвестно" — тогда используется общее среднее.
 */
#include <stdbool.h>

void  energy_init(void);
void  energy_tick(bool on_battery, float load_w, float dt_s, int hour);
float energy_hour_w(int hour);                       // средняя нагрузка в этот час, Вт
bool  energy_hour_known(int hour);
float energy_mean_w(void);                           // общее среднее, Вт
bool  energy_mean_known(void);
float energy_need_wh(int hour, int minute, float hours);          // нужно энергии на N часов вперёд
float energy_runtime_h(float avail_wh, int hour, int minute, float max_h);  // на сколько хватит
void  energy_reset(void);
