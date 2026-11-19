# JK BMS → Telegram (ESP32-S3, ESP-IDF 5.5.1)

Контроллер зарядки 13S NMC по напряжению JK BMS + Telegram-бот + веб-настройки.
Общение с пользователем — на русском.

- Описание для пользователя: `README.md`
- История разработки, требования, принятые решения и TODO: `docs/HISTORY.md` — прочитай перед работой.
- Сырая история диалога (не в git): `.claude-history/*.jsonl`

## Важные договорённости
- Пороги заряда задаются на ячейку (4.05 off / 3.90 on), пакет = порог × cells.
- Защиту по максимальной ячейке НЕ делать — это делает BMS.
- Failsafe: нет BMS или нет сети → реле OFF. Task WDT 60 с → перезагрузка (60 — максимум в ESP-IDF 5.5; больше Kconfig молча отбрасывает и ставит 5 с, тогда плата перезагружается через 6.5 с после старта).
- Секреты только в `main/secrets.h` (gitignored) или NVS; токены и пароли не коммитить.

## Сборка
```powershell
. "$env:IDF_PATH\export.ps1"
idf.py build
idf.py -p COMx flash monitor
```
Если `export.ps1` ищет несуществующее python-окружение, задай `IDF_PYTHON_ENV_PATH` на существующее
(`%USERPROFILE%\.espressif\python_env\idf5.5_py3.11_env`) и добавь его `Scripts` в PATH.
