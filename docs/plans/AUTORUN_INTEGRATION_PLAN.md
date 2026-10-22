# AutoRun PC Game Integration Plan

План интеграции базы совместимости Windows-игр, автоматической конфигурации и 1-tap установки с RuTracker в среду AutoRun (Wine-NX) на Nintendo Switch через `pipensx`.

---

## 1. Цель и конечный результат продукта

**Конечный результат:**
Полноценный рабочий продукт в клиенте `pipensx` на Nintendo Switch:
1. Пользователь открывает раздел **PC Games / AutoRun** в интерфейсе консоли.
   - **Авто-проверка среды (Bootstrap):** если среда AutoRun ещё не установлена на Switch (`switch/wine/wine-nx-runtime.nro` отсутствует), pipensx сообщает об этом и предлагает **установить AutoRun в один клик** (скачивает актуальный бандл runtime, Mesa/Box64/Wine и разворачивает на microSD).
2. Видит витрину проверенных игр с постерами, бейджами производительности (🟢 60 FPS, 🟡 30 FPS), описанием и рекомендуемым разгоном.
3. Нажимает **«Установить» (One-Tap)**:
   - Игра скачивается напрямую в целевую папку `sdmc:/switch/wine/drive_c/Games/<Game>/` (без двойного расхода места на SD-карте) через активный транспорт (P2P BitTorrent или Debrid-кэш).
   - Автоматически создаётся оптимизированный `dxvk.conf`.
   - Автоматически и безопасно вносятся твики в реестр Wine (`user.reg`).
   - Создаётся ярлык на рабочем столе Wine (`drive_c/users/wine/Desktop/`).
   - Консоль предлагает в один клик вынести иконку игры на **домашний экран Switch (Home Menu)** через stub-NSP форвардер и прописать профиль разгона в **sys-clk**.
4. Пользователь запускает игру в любое удобное время.

---

## 2. Структура директорий на Nintendo Switch (SD Card)

AutoRun располагается в `/switch/wine/`:

| Путь на SD-карте | Назначение |
|---|---|
| `switch/wine/wine-nx-runtime.nro` | Главный исполняемый файл / лаунчер Wine-NX |
| `switch/wine/drive_c/` | Виртуальный диск `C:` Windows |
| `switch/wine/drive_c/Games/<Game>/` | Папка установленной игры |
| `switch/wine/drive_c/users/wine/Desktop/` | Рабочий стол Wine (сюда кладётся `.lnk` ярлык) |
| `switch/wine/drive_c/users/wine/Documents/` | Настройки игр, сейвы и конфиги |
| `switch/wine/registry/user.reg` | Пользовательский реестр Wine (разрешение, геймпад, твики) |
| `switch/wine/registry/system.reg` | Системный реестр Wine |
| `switch/wine/drive_c/Games/<Game>/dxvk.conf` | Оптимизированный конфиг DXVK (async шейдеры, потоки) |
| `config/sys-clk/config.ini` | Конфигурация оверклока Switch (профили частот по Title ID) |

---

## 3. Фазы реализации

### Фаза 1: Сбор первичных данных совместимости (Завершено ✅)
- **Парсер Telegram (`tools/autorun/sync_telegram.py`):** выгрузка сообщений из топика 275 канала `@AutoRunn`.
- **ИИ-экстрактор (`tools/autorun/ai_extractor.py`):** парсинг отчетов тестеров (FPS, оверклок, рендерер, транслятор FEX/Box64, твики реестра).
- **База данных:** сформирован `resources/autorun/compatibility_db.json` (86+ протестированных игр).

---

### Фаза 2: CLI-прототип деплоя на ПК (`tools/autorun/deploy_game.py`) (Завершено ✅)
- Проверен механизм генерации `dxvk.conf` и инжекта блоков реестра в `user.reg` (протестировано на *Deus Ex: Human Revolution*, *Barnyard*, *FlatOut 2*).
- Подтверждена работоспособность структуры каталогов `drive_c/Games/<Title>`.

---

### Фаза 3: Пайплайн каталога и подбора раздач RuTracker (`pipensx-catalog`) (Завершено ✅)

**Цель:** Обогатить базу проверенных игр прямыми ссылками на рабочие **Portable-раздачи** с RuTracker и экспортировать их в единый `catalog.json`.

1. **Инструмент поиска, верификации и синхронизации раздач (`tools/autorun/curate_torrents.py`):**
   - Утилита для верификации и маппинга проверенных раздач на совместимые игры:
     * Проверка чистоты релиза (portable / unpack-and-play, отсутствие `setup.exe`, инсталляторов и `.iso`).
     * Верификация формата главного исполняемого файла (`FlatOut2.exe`, `DXHR.exe`, `Barnyard.exe` и др.).
     * Проверка целевых папок, корректности профилей оверклока, конфигураций `dxvk.conf` и секций реестра `user.reg`.
   - Проверены и зафиксированы все текущие протестированные раздачи вручную (без внешних LLM-валидаторов) в `resources/autorun/curated_torrents.json` (20 готовых к установке PC-игр).
2. **Расширение схемы `pipensx-catalog` (`schema.json` v2):**
   - Добавлены поля `type: "autorun"`, `category: "PC Ports"`, а также вложенный блок `autorun`:
     ```json
     {
       "title": "FlatOut 2",
       "type": "autorun",
       "category": "PC Ports",
       "info_hash": "10B83EF3082FA6CF29D81D227C3DAFCFE5165C01",
       "size": 2847592812,
       "size_bytes": 2847592812,
       "cover": "https://...",
       "autorun": {
         "executable": "FlatOut2.exe",
         "target_folder": "FlatOut2",
         "status": "playable",
         "fps": "60",
         "overclock": "1785/768/1900",
         "translator": "Box64",
         "renderer": "DXVK",
         "notes": "Стабильные 55-60 FPS с DXVK Sarek. Управление геймпадом нативно, твик разрешения 720p через реестр.",
         "dxvk_conf": "dxvk.enableAsync = true\ndxvk.numCompilerThreads = 4\ndxvk.maxChunkSize = 32\nd3d9.maxFrameLatency = 1\ndxgi.maxFrameLatency = 1\n",
         "registry": "[Software\\\\Bugbear\\\\FlatOut2]\n\"DisplayWidth\"=dword:00000500\n\"DisplayHeight\"=dword:000002d0\n\"Fullscreen\"=dword:00000001\n"
       }
     }
     ```
3. **Экспорт и тесты:**
   - Обновлены `export_catalog.py` и `build_catalog.py` для сквозной поддержки и валидации метаданных `autorun`.
   - Добавлены unit-тесты (`tests/test_export_catalog.py`). Все 61 тест в `pipensx-catalog` пройдены успешно.
   - Экспортированы 20 проверенных PC-игр в релизные `catalog.json` и `manifest.json`.

---

### Фаза 4: Движок скачивания, AutoRun Runtime Bootstrap и деплой игр в `pipensx` (Switch Core) (Завершено ✅)

**Цель:** Обеспечить наличие среды AutoRun на консоли и реализовать прямую потоковую установку игр в файловую систему Wine на microSD без промежуточного дублирования файлов.

1. **AutoRun Runtime Bootstrap (Автоустановка среды при её отсутствии) (`src/app/autorun_service.cpp`):**
   - **Pre-flight проверка:** реализована проверка наличия рантайма (`sdmc:/switch/wine/wine-nx-runtime.nro`) и чтение установленной версии (`version.json` / `version.txt`).
   - **One-Click инсталлятор среды:**
     * Сервис загрузки и распаковки рантайма `AutorunService::installRuntimeArchive` / `downloadAndInstallRuntime` с потоковой распаковкой архивов ZIP (`extractRuntimeZip`) и колбэками прогресса.
     * Автоматическая инициализация структуры каталогов `prepareAutorunEnvironment`: `drive_c/Games/`, `drive_c/users/wine/Desktop/`, `drive_c/users/wine/Documents/`, создание дефолтных `user.reg` и `system.reg`.
   - **Проверка обновлений рантайма:** реализован метод `AutorunService::checkForUpdates` и парсер релизов GitHub API `parseRelease` со сравнением SemVer версий.

2. **Режим установки `TransferMode::AutoRunInstall` (`src/app/download_manager.hpp`, `src/app/download_manager.cpp`):**
   - Добавлен режим `TransferMode::AutoRunInstall`.
   - Загрузка данных сразу в целевую папку `autorunGamesRoot` (`sdmc:/switch/wine/drive_c/Games/<target_folder>/`).
   - Поддержка сохранения, персистентности в очереди (`queue.bencode` с режимом `"autorun"`), безопасного удаления и очистки хранилища (`isManagedChild`).
   - Использование общего транспорта: P2P BitTorrent (`libutp`/`dht`) и Debrid (`TorBox`, `Real-Debrid`, `TorrServer`).
   - Интеграция с `CatalogService` (парсинг блока `autorun` из `catalog.json` в `AutorunProfile`) и 1-tap установкой в `ReleaseAdder` / `OneTapPlan`.

3. **Предварительная валидация раздачи (`AutorunValidation`):**
   - Метод `validateAutorunFilePaths` / `validateAutorunRelease` анализирует список файлов торрента.
   - Детектирует установщики (`setup.exe`, `install.exe`, `installer.exe`, `unins000.exe`) и дисковые образы (`.iso`, `.img`, `.mdf`, `.nrg`, `.cue`), предупреждая о необходимости portable-версии.
   - Находит и валидирует главный исполняемый файл `.exe`.

4. **Post-Deploy Hook (`src/app/switch_deploy.cpp`, `src/app/autorun_service.cpp`):**
   - Автоматический и ручной вызов post-deploy хука при завершении загрузки задачи `TransferMode::AutoRunInstall`:
     1. Создание оптимизированного `dxvk.conf` в папке игры (с поддержкой индивидуального конфига из профиля либо дефолтного с async-компиляцией).
     2. Идемпотентный мердж секции реестра в `sdmc:/switch/wine/registry/user.reg` (`mergeUserRegistry`) без дублирования существующих веток.
     3. Генерация бинарного Windows Shell Link (.lnk) ярлыка (`generateShellLink`) и сохранение на рабочий стол Wine (`sdmc:/switch/wine/drive_c/users/wine/Desktop/<Game>.lnk`).
     4. Запись квитанции развертывания (`receiptPath`) для идемпотентного отслеживания статуса.

---

### Фаза 5: Интеграция с Home Menu (Stub-NSP) и `sys-clk`

**Цель:** Предоставить пользователю возможность запускать игру с главного экрана Switch с автоматическим разгоном.

1. **Генерация Stub-NSP форвардера на лету:**
   - Легковесный шаблон NSP-форвардера, собираемый прямо в памяти консоли:
     * **Title ID**: детерминированный хэш от названия игры с базовым префиксом хоумбрю (например, `0100BEEFxxxxxxxx`), исключающий конфликты с официальными играми.
     * **NACP (`control.nacp`)**: заголовок с названием игры и автором («AutoRun / pipensx»).
     * **Иконка**: обложка игры из каталога, отмасштабированная в JPEG 256x256.
     * **Payload**: код вызова `envSetNextLoad` с аргументом запуска среды Wine и целевого `.exe`.
   - Установка через встроенный `InstallBackend` (`ncm`/`es`).
2. **Интеграция с `sys-clk`:**
   - Проверка наличия `/config/sys-clk/config.ini`.
   - Если файл есть: запрос у пользователя *«Применить рекомендуемый профиль разгона в sys-clk?»*.
   - При согласии — добавление секции с Title ID игры и частотами (`cpu=1785`, `gpu=768`, `mem=1900`). При выходе из игры частоты сбрасываются автоматически на системные.

---

### Фаза 6: Пользовательский интерфейс Switch (Borealis UI)

**Цель:** Создать удобную и понятную витрину каталога с 1-tap установкой.

1. **Витрина PC-игр (`src/ui/catalog/catalog_view.cpp`):**
   - Вкладка / полка **«PC Games (AutoRun)»** рядом с разделами нативных игр.
   - Баннер готовности среды: если AutoRun не установлен — яркая кнопка в шапке *«Установить AutoRun Runtime»*, блокирующая ошибки запуска.
   - Бейджи совместимости:
     * 🟢 **60 FPS** (Playable / Verified)
     * 🟡 **30 FPS** (Playable with tweaks)
     * 🔴 **Issues** (Требуются эксперименты)
   - Карточка деталей игры:
     * Постер, скриншоты, описание.
     * Блок характеристик: рекомендуемый транслятор (Box64/FEX), рендерер (DXVK), частоты оверклока.
2. **Диалог после установки:**
   - После завершения деплоя показ информативного окна:
     * *«Игра <Title> готова к запуску!»*
     * Чекбокс / кнопка: `[Создать ярлык в главном меню Switch]` (по умолчанию включено).
     * Чекбокс / кнопка: `[Применить профиль разгона в sys-clk]` (по умолчанию включено).

---

### Фаза 7: Тестирование и верификация

1. **Тесты бэкенда (`tests/test_autorun_deploy.cpp`):**
   - Парсинг структуры `AutorunProfile` из `catalog.json`.
   - Мердж реестра в `user.reg` (проверка отсутствия повреждений существующего содержимого).
   - Генерация `dxvk.conf`.
   - Формирование Stub-NSP и проверка заголовков PFS0/NACP.
2. **Контрольный сквозной тест на консоли (3 эталонных тайтла):**
   - *FlatOut 2* (60 FPS, нативная поддержка контроллера, твик разрешения 720p).
   - *Deus Ex: Human Revolution* (твик соотношения сторон 16:9 и разрешения 960x540).
   - *Barnyard* (твик включения геймпада в реестре).
   - Проверка: скачивание -> применение твиков -> создание иконки в Home Menu -> автоматический оверклок при запуске.

---

## 4. Конфиденциальность и безопасность публикации данных

- База совместимости и ссылки на торренты распространяются исключительно в анонимном виде через релизы репозитория `pipensx-catalog` (GitHub Releases / Gist).
- Личные Google-аккаунты и Telegram-сессии не должны фигурировать в коде, манифестах или коммитах.
