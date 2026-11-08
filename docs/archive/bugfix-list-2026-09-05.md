# Список фиксов по экспорту чата 2026-09-05

Источник: `/home/i3sey/Downloads/Telegram Desktop/ChatExport_2026-09-05/result.json` (1060 сообщений, 23.08–05.09),
фото `photos/photo_*.jpg`, QR-логи `9E82` (Clown), `8CE1` (Anigma). Оба QR-лого обрезаны
(`только последние логи`), строк краша в них нет — диагноз по скринам и тексту чата.
Релиз у юзеров: 1.4.4. Пофикшенное (в т.ч. фиксы в коде без релиза:
OOM piece buffer, место под NSZ, краш Dying Light, апдейтер `File exists`)
в список не входит — ниже только баги без фикса, воспроизводимые на 1.4.4.
- Ниже только то, что можно понять и починить. Хотелки (чат в приложении,
  комменты, AllDebrid, поиск по жанрам, фоновая качалка в игре) сюда не входят.

---

## B1. Краш/выключение → прогресс в ноль + мусор на SD
- Симптомы:
  - Clown: Hitman 4 ГБ → вылет + съедено 20 ГБ, прогресс не сохранился; TF2-порт вылет
    через 10 сек + подъедает карту (QR 9E82 пустой, errors=0 — лог обрезан).
  - AIMOLL: MK11 15 ГБ дважды в ошибку, «Прочие» не чистятся.
  - Ян: было 35 ГБ → стало 8 ГБ, `switch/pipensx/downloads` по USB пустая, помог только DBI Clean.
  - ASM, H J: неудачные загрузки занимают место, непонятно как убрать.
  - Пауза сносит файлы (white), BIG CHIEF `pause restarts download`, GH #72
    `Download progress resets on turning switch off`, GH #76 `Error halfway`.
- Код: `src/app/download_manager.cpp` (save/load/resume, `clearCompleted`),
  `src/app/install_journal.*`, `src/platform/storage.c`, Storage Manager
  (`2e9c26f`, `4e7f078`).
- Починить:
  1. resume после краша/выключения (журнал_piece/verified, не качать заново);
  2. пауза ≠ удаление данных;
  3. кнопка/авточистка оборванных `downloads/<hash>` + показ «мусора» в цифрах.
- Статус 05.09 (ветка `fix/b1-resume-garbage`): п.3 закрыт `e433694`
  (orphanDownloadBytes/clearOrphanDownloads + строка в Storage);
  п.1 закрыт `397d72b` (чекпоинт bitfield+прогресса каждые 5 с в торрент-лупе
  и в debrid onProgress, тест crash-checkpoint в test_manager);
  п.2 по коду уже так (pause только меняет статус, данные на месте) —
  репорты «пауза сносит» объясняются потерей bitfield/прогресса при краше,
  что чинит чекпоинт. Проверки: `make -f Makefile.pc test`, `make pc`,
  `make switch` — зелёные; `make golden` — зелёный (матрица ужата `fe9c79f`).
- Проверка: оборвать закачку → место возвращается/прогресс продолжается; `make -f Makefile.pc test`.

## B2. Порт падает на `cannot mkdir` с не-ASCII путём
- Симптом: Tomodachi Life (Uhartim, 03.09, photo_149):
  `cannot mkdir for '.../Tomodachi_Life.../Russian Machine Translation 0.9 (Text)/atmosphere/contents/английская озв': No such file or directory`,
  `6.3/6.4 GiB pkg 2/2 Error`. Игра поставлена, РУ-озвучка — нет.
- Код: `src/app/switch_deploy.cpp`, `src/app/port_archive.cpp` — после
  `81474d4 unify port transaction`, `920db3f LayeredFS`, `bc2518a copy large ports`
  всё ещё нет рекурсивного mkdir / нормализации unicode-путей.
- Починить: рекурсивный `mkdir -p` для deploy-путей + тест с кириллицей/пробелами/точками.
- Статус 05.09 (ветка `fix/b1-resume-garbage`): закрыт `a9e3986`. Ошибка шла из
  движка торрент-хранилища (`src/platform/storage.c`, не SD-деплой): все три копии
  `mkdirs` считали EEXIST успехом без проверки «это каталог» — файл на месте
  каталога давал криптичный ENOENT глубже; теперь там ENOTDIR на блокирующем
  компоненте. Плюс `ensure_disk_file_open` уходит в короткий `_files/`-фолбэк
  и при ошибке mkdir (раньше — только при ошибке open), `locate` оба расклада
  уже резолвит. Тесты: unicode-вложенность + блокирующий файл с фолбэком
  (второй без фикса падает). Проверки: `make -f Makefile.pc test`,
  `make switch` — зелёные.
- Проверка: новый unit-тест на вложенный путь с `английская озв`; `make -f Makefile.pc test`.

## B3. Updates hub показывает мусорные версии
- Симптом: photo_136 (Iván): FF IV `v327680 → v458752`, FF V `v393216 → v458752`,
  Pokémon `v131072 → v458752` — одинаковый target. 👾 03.09: «установленная версия —
  рандомные цифры; игры с обновлениями не в Обновлениях». Iván: «update ведёт на
  скачивание всей игры, выбирает всё».
- Код: `src/app/game_update_service.cpp`, `src/app/installed_title_service.cpp`,
  `src/app/game_update_install.*`, экран Installed/Updates.
- Починить: маппинг installed titleID → catalog version (не путать тайтлы);
  smart-install «только апдейт» по умолчанию; тест на 3 игры выше.
- Статус 06.09 (ветка `fix/b1-resume-garbage`): закрыт `5cdb60d`.
  `catalogEntryForTitle` брал newest-published бандл (часто базу) без учёта
  версии — теперь `preferVersionMatch` выбирает бандл с foundVersion чека
  (иначе legacy-пик); foundVersion тянут из меню строки. С правильным
  бандлом smart-маска предвыбирает только апдейт. Строки хаба показывают
  x.y.z (`v5.0.0`) вместо сырого decimal (`v327680`) с фолбэком на raw.
  Тест: 3 тайтла × (база+апдейт), per-title таргеты. Проверки:
  `make -f Makefile.pc test`, `make golden` (без ре-бейзлайна, в бюджете),
  `make switch`, `scripts/check_i18n.py` — зелёные.
- Проверка: FF4/FF5/Pokémon показывают разные target; `make golden` (Installed/Updates).

## B4. Мультифайл/DLC/debrid ставится через раз
- Симптомы:
  - Muq (1.4.1→1.4.3, photo_72-76/79-82/101-104): 1 файл ок, 7 файлов Diablo 3 →
    `Package is not a PFS0 NSP/NSZ` без скачивания; LOTR 12 ГБ скачался →
    `installer rejected the downloaded data`; удаление для перекачки → краш.
  - QUALITY CONTROL: debrid мультифайл → error; после базы только `View download`,
    DLC отдельно не взять.
  - Mesh: RD paid не качает, swarm — ок. GH OPEN: #77 `Real-Debrid SSL connect error`
    (1.4.4), #75 `Torrserver: Torrenting disabled, но тоггла нет`, #63, #78.
- Код: `src/app/debrid_transfer.cpp`, `src/app/realdebrid_*`, `src/install/install_backend_switch.cpp`,
  `src/app/package_coordinator.hpp`, `src/app/catalog_batch_installer.*`.
  Есть `85264aa sequential RD`, `3d94ab9 file cap`, но кейсы выше живы.
- Починить:
  1. мультипакет: какой именно файл не PFS0 — в ошибку (имя+размер), а не общий текст;
  2. DLC-докачка после базы без удаления загрузки;
  3. #75: либо убрать метод Torrserver, либо добавить тоггл, на который ссылается ошибка.
- Статус 06.09 (ветка `fix/b1-resume-garbage`): п.1 закрыт `783a84a`
  (`Package '<path>' (<bytes> bytes): <error>` в PackageCoordinator и
  debrid attemptStreamInstall + тест webpage-as-NSP); п.3 закрыт `dc0684a`
  (ошибка называет точный тоггл «Direct BitTorrent», Settings/Download
  source; убирать TorrServer не стали). П.2 отложен как фича:
  смены fileSelection после импорта нет ни в менеджере, ни в UI — нужен
  отдельный дизайн (torrent re-check vs debrid re-fetch). Проверки:
  `make -f Makefile.pc test`, `make switch` — зелёные.
- Проверка: Diablo 3 (7 файлов) и база+DLC по шагам; `make -f Makefile.pc test`.

## B5. Сон/скринсейвер убивает закачку, бывают зависания системы
- Симптомы: Павел (сон), Clown «экран затухает → ошибка», Макс 05.09
  `Screensaver зависший, в emunand не грузится`, Delevoy «начал вешать систему».
  В работе незакоммиченное: `src/ui/common/burn_in_saver.hpp`,
  `src/platform/switch_backlight.*`, `src/main_switch.cpp`, `settings_panels`.
- Починить: выкл экрана без сна торрента; пробуждение кнопкой не должно убивать закачку;
  вотчдог от вешания системы. Это отдельная фича-ветка, не смешивать с B1–B4.
- Проверка: длинная закачка с гашением экрана; `make switch` + `make pc`.

## B6. Обновление игры ломает запуск
- Симптомы: Ян Broforce после обновы — системное `Программа закрыта...` (photo_152);
  Исмаил Zelda BOTW — обновил → не запускается, переустановка не помогла
  (подозрение на моды/прошивку, dev просил скрин DBI — не дожали).
- Код: `src/app/game_update_install.*`, `src/app/installed_title_service.cpp`.
- Починить минимум: перед обновой показать установленную версию/SDK-моды;
  после фейла — внятная ошибка «обнова X требует HOS Y / конфликтует с модом»,
  а не молчаливая поломка запуска.
- Проверка: кейсы BOTW/Broforce с моком installed-версий; `make -f Makefile.pc test`.

## B7. Каталог не обновляется + фильтры + тормоза списка
- Симптомы: Константин 29.08 на 1.4.4 «каталог с 27 числа, фильтрация перестала
  работать» (QR 409E не декодируется — нужен перехват); Кирилл «не обновляется ни на
  каких сетях»; 👾 «бесконечный список Экшенов, скролл 10-15 fps»; 2DRoach
  «обложки не грузятся».
- Код: `src/app/catalog_refresh.*`, `catalog_service`, `game_metadata_service`,
  UI списка (есть `0479c87 GitHub TLS CA`, но репорты после него).
- Починить: показать честную дату/статус каталога («каталог не обновлялся» заметнее),
  починить фильтр, виртуализацию списка + кеш обложек.
- Проверка: `scripts/check_i18n.py` для новых строк; `make golden`.

---

## Порядок
1. B1 (resume/мусор — больше всего сообщений).
2. B2 (маленький, конкретный, со скрином-ошибкой).
3. B4 (debrid/мультифайл, есть GH-ишьюсы #75/#77).
4. B3 → B6 → B7 → B5 (хабы/каталог/сон — крупные, отдельно).

## Проверки перед закрытием каждого пункта
- `make -f Makefile.pc test` для shared/core (src/core, src/app).
- `make golden` для UI-строк/экранов.
- `make switch` для Switch-кода + `make pc` для CLI (по AGENTS.md одного билда мало).
- Новые строки локалей: сначала `en-US`, потом `ru/pt-BR/fr`, затем `scripts/check_i18n.py`.
- QR-логи триажить по `.agents/skills/bug-report/SKILL.md`, логи целиком не читать.
