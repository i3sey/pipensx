# AutoRun PC Game Compatibility & Deployment Suite for pipensx

Этот набор инструментов предназначен для сбора, обработки и развёртывания протестированных компьютерных игр на Nintendo Switch через эмулятор **AutoRun** (Wine + Box64 / FEX).

Инструменты синхронизируются с темой в Telegram ([t.me/AutoRunn/275](https://t.me/AutoRunn/275)), поддерживают локальные нейросети (LM Studio / Ollama) для парсинга комментариев и позволяют разворачивать игры вместе с необходимыми конфигами (`dxvk.conf`, `user.reg`, библиотеки и твики) прямо на SD-карту свитча.

---

## 📁 Структура файлов

```text
~/pipensx/
├── resources/autorun/
│   ├── compatibility_db.json      # Полная база игр в формате JSON
│   ├── compatibility_db.csv       # Экспорт в CSV
│   ├── compatibility_db.xlsx      # Оформленная таблица Excel с цветовой маркировкой
│   └── google_sheets_token.json   # Токен авторизации Google Sheets/Drive
└── tools/autorun/
    ├── autorun_db.py              # Модуль работы с базой совместимости
    ├── ai_extractor.py            # Парсер (Regex эвристика + локальная нейросеть)
    ├── sync_telegram.py           # Синхронизация с веткой t.me/AutoRunn/275
    ├── deploy_game.py             # Скрипт установки игры и конфигов на Switch
    └── export_google_sheets.py    # Экспорт и авто-оформление Google Таблицы
```

---

## 🚀 Быстрый старт

### 1. Просмотр и поиск по базе совместимости
```bash
python3 tools/autorun/autorun_db.py
```
Или используйте в Python-коде:
```python
from tools.autorun.autorun_db import AutorunDB

db = AutorunDB()
games = db.find("Deus Ex")
for g in games:
    print(g["status_badge"], g["game_title"], g["fps"], g["overclock"])
```

### 2. Установка игры прямо в AutoRun на Nintendo Switch
Скрипт `deploy_game.py` копирует файлы игры в каталог `switch/wine/drive_c/Games/<GameName>`, автоматически прописывает оптимизированный `dxvk.conf` и внедряет твики реестра в `switch/wine/registry/user.reg` (например, исправление растяжения экрана для Deus Ex, включение геймпада для Barnyard и т.д.):

```bash
# Тестовый прогон без записи (Dry Run)
python3 tools/autorun/deploy_game.py --game "Deus Ex" --src "/path/to/game" --sd-root "/run/media/user/SWITCH" --dry-run

# Реальная установка на SD-карту
python3 tools/autorun/deploy_game.py --game "FlatOut 2" --src "~/Downloads/FlatOut2" --sd-root "/run/media/i3sey/SWITCH"
```

### 3. Синхронизация новых отчётов из Telegram
Скрипт подключается через существующую локальную сессию Telethon (`~/.config/goose/telegram.session`), забирает новые отзывы и обновляет базу:

```bash
# Стандартный режим (быстрый шаблонный парсер)
uv run --with telethon --with python-socks python3 tools/autorun/sync_telegram.py

# С участием локальной нейросети (LM Studio на http://localhost:1234/v1)
uv run --with telethon --with python-socks python3 tools/autorun/sync_telegram.py --ai
```

### 4. Экспорт в Google Таблицу
1. **Автоматически через Google Sheets API**:
   ```bash
   uv run --with google-auth-oauthlib --with google-api-python-client python3 tools/autorun/export_google_sheets.py
   ```
   *(После разового включения Google Sheets API в консоли разработчика создает и оформляет таблицу с цветами и автошириной колонок).*

2. **В 1 клик через функцию =IMPORTDATA**:
   Создайте новую таблицу [sheets.new](https://sheets.new) и вставьте в ячейку **A1**:
   ```excel
   =IMPORTDATA("https://gist.githubusercontent.com/i3sey/27426c490496825cd573f1f0d5549278/raw/compatibility_db.csv")
   ```

3. **Импорт готового файла Excel**:
   Файл `resources/autorun/compatibility_db.xlsx` уже полностью отформатирован со всеми цветами (🟢 зеленый, 🟡 желтый, 🔴 красный) и закрепленной шапкой.
