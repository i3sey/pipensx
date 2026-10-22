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

### Фаза 5: Интеграция с Home Menu (Stub-NSP) и `sys-clk` (Завершено ✅)

**Цель:** Предоставить пользователю возможность запускать игру с главного экрана Switch с автоматическим разгоном.

1. **Генерация Stub-NSP форвардера на лету (`src/app/autorun_service.cpp`, `src/autorun/autorun_forwarder.c`):**
   - **Title ID**: детерминированный генератор `generateAutorunTitleId` на основе названия игры с префиксом `0x0100BEEF` и стандартным окончанием `0000` (например, `0100BEEFxxxx0000`), исключающим конфликты с играми Nintendo.
   - **NACP (`generateNacp`)**: создание структуры NACP 0x4000 байт с названием игры, автором («AutoRun / pipensx»), версией «1.0.0» и всеми поддерживаемыми языками.
   - **Иконка**: обложка игры в формате JPEG 256x256 (с поддержкой пользовательской обложки или встроенного дефолтного JPEG).
   - **Payload форвардера (`src/autorun/autorun_forwarder.c`)**:
     * Компактный исполняемый файл для Nintendo Switch, компилируемый в `romfs:/autorun-forwarder.nro`.
     * При запуске из Home Menu считывает собственный Title ID через `svcGetInfo(..., InfoType_ProgramId, ...)`, загружает параметры из `sdmc:/switch/wine/forwarders/<TitleID>.ini` и вызывает `envSetNextLoad` с запуском Wine-NX и целевого `.exe`.
   - **Сборщик NSP и PFS0 (`buildPfs0`, `parsePfs0`, `generateAutorunStubNsp`)**:
     * Генерация контейнера PFS0 в оперативной памяти с Program NCA, Control NCA (NACP + icon), CNMT NCA (структура `NcmExtPackagedContentMetaHeader` + `NcmPackagedContentInfo`) и сырыми метаданными.
   - **Установка форвардера (`installAutorunForwarder`)**:
     * Потоковая передача сгенерированного пакета в абстракцию `InstallBackend` (`ncm`/`es` на Switch, файловая симуляция на PC) с автоматическим сохранением конфигурации форвардера на SD.

2. **Интеграция с `sys-clk` (`autorunSysClkConfigPath`, `parseOverclockProfile`, `applySysClkProfile`):**
   - **Детектирование:** метод `isSysClkInstalled` проверяет наличие `/config/sys-clk/config.ini` или каталога сисмодуля sys-clk в Atmosphere.
   - **Парсер профилей разгона:** `parseOverclockProfile` распознает различные форматы указания частот (`1785/768/1900`, `1963 / 1152 / 2133`, `CPU 1785, GPU 614, RAM 1900`, `Safe Max`, `cpu=..., gpu=..., mem=...`) и приводит их к структуре `SysClkProfile`.
   - **Идемпотентная конфигурация:** `applySysClkProfile` и `removeSysClkProfile` безопасно добавляют, обновляют или удаляют секцию `[<TitleID>]` в `/config/sys-clk/config.ini` с настройками частот для портативного и док-режимов (`handheld_cpu`, `handheld_gpu`, `handheld_mem`, `docked_cpu`, `docked_gpu`, `docked_mem`) без повреждения существующих профилей других игр и комментариев.

3. **Верификация и тесты:**
   - Добавлены тесты генерации Title ID, NACP, PFS0, Stub-NSP и манипуляций профилями `sys-clk` в `tests/test_autorun.cpp`.
   - Создан отдельный тестовый модуль `tests/test_autorun_deploy.cpp` (парсинг `AutorunProfile`, мердж реестра, `dxvk.conf`, заголовки PFS0/NACP). Все тесты успешно пройдены на PC и Switch.

---

### Фаза 6: Пользовательский интерфейс Switch (Borealis UI) (Завершено ✅)

**Цель:** Создать удобную и понятную витрину каталога с 1-tap установкой.

1. **Витрина PC-игр (`src/ui/catalog/catalog_view.hpp`, `src/ui/catalog/catalog_view.cpp`):**
   - Добавлен раздел `CatalogSection::AutoRun` и вкладка навигации **«PC Games (AutoRun)»** (`NavIconType::AutoRun`) в шапке `MainFrame` рядом с разделами нативных игр и портов, а также полка в `HomeView` (`Slot::Kicker::AutoRun`).
   - **Баннер готовности среды:** если среда AutoRun не установлена на microSD (`!isAutorunInstalled("sdmc:")`), в витрине отображается адаптивный баннер с предупреждением и яркой кнопкой *«Установить AutoRun Runtime»*, инициализирующий среду или скачивающий актуальный рантайм.
   - **Pre-flight блокировка ошибок запуска:** при попытке установки PC-игры без рантайма приложение перехватывает действие и предлагает установить среду AutoRun в один клик.
   - **Бейджи совместимости:**
     * 🟢 **60 FPS** / Playable (Verified)
     * 🟡 **30 FPS** / Tweaks (Playable with tweaks)
     * 🔴 **Issues** / Unplayable (Требуются эксперименты)
     Отображаются прямо на карточках витрины каталога (`autorunCompatibilityBadge`).
   - **Карточка деталей игры (`src/ui/detail/game_detail.hpp`):**
     * Постер, скриншоты, описание из заметок совместимости.
     * Блок характеристик: платформа Windows (AutoRun / Wine-NX), бейдж совместимости, рекомендуемый транслятор (Box64/FEX), рендерер (DXVK/WineD3D), частоты разгона sys-clk, исполняемый `.exe` и целевая папка.

2. **Диалог после установки (`src/ui/detail/autorun_post_install_dialog.hpp`):**
   - Интеграция с циклом деплоя `SwitchDeployService` через `PendingOffer` (`autorunPostDeploy = true`).
   - По завершении деплоя задачи AutoRun отображается окно настройки запуска:
     * *«Игра <Title> готова к запуску!»*
     * Чекбокс `brls::BooleanCell`: `[Создать ярлык в главном меню Switch]` (по умолчанию включено).
     * Чекбокс `brls::BooleanCell`: `[Применить профиль разгона в sys-clk]` (по умолчанию включено).
   - При подтверждении автоматически генерируется и устанавливается forwarder Stub-NSP и записывается секция оверклока в `/config/sys-clk/config.ini`.
   - В окне `DetailsActivity` добавлена кнопка повторного вызова диалога для завершенных AutoRun-задач.
   - Поддержка полной локализации во всех 8 языковых пакетах (`en-US`, `ru`, `pt-BR`, `fr`, `es`, `it`, `ko`, `zh-CN`).

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
