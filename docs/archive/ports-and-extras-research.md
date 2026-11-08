# Порты, extras и переводы: исследование и список починок

Дата: 2026-09-21.
Статус: исследование по текущему коду (`main`), тестам и закрытым issues
(#6, #55, #80). Это не исторический план из `docs/plans/` и не roadmap:
ниже — фактическое поведение и конкретные места, которые надо чинить.

Связанные коммиты:

- `920db3f` — LayeredFS + NSP как транзакция деплоя
- `81474d4` — единая port-транзакция (скачать всё, потом задеплоить)
- `343524f` — **намеренно** пропускать extras у StreamInstall, чтобы
  дамп «NSP + русификатор» не падал на 100%

---

## 1. Короткий вывод

У pipensx **два разных продукта в одном установщике**, и граница между ними
проведена по имени файлов, а не по тому, что пользователь хочет получить.

| Что пользователь ставит | Как pipensx это видит | Что происходит |
|---|---|---|
| NSP/NSZ игры | `StreamInstall` | Пакеты стримятся в NCM. Всё остальное — extra, по умолчанию Skip |
| NRO-порт (`switch/Game/Game.nro`, zip/7z с NRO) | `PortInstall` | Скачать на диск → `SwitchDeployService` копирует в `/switch` |
| LayeredFS-only (`atmosphere/contents/<tid>/romfs/…`) | `PortInstall` | Перенос на корень SD |
| **Игра + перевод / русификатор / LayeredFS в том же торренте** | **игра**, не порт | One-tap ставит только NSP. Перевод не качается и не ставится |
| Zip русификатора рядом с NSP | extra, не порт | Даже в «Выбрать файлы» debrid zip не скачает; P2P zip ляжет в downloads и **не распакуется**, если внутри нет NRO |

Это ровно баг из #6 («порты не копируются в `\switch`, папка downloads пустая,
не видно куда легли файлы»), плюс массовый кейс RuTracker: игра и перевод в
одном релизе.

`343524f` правильно защитил установку NSP от падения на мусорных extras
(readme, битый zip). Цена: **любой полезный extra стал невидимым для one-tap
и почти невидимым для деплоя**.

---

## 2. Как система устроена сейчас

### 2.1 Три режима

`TransferMode` в `src/app/download_manager.hpp`:

- `DownloadOnly` — все выбранные файлы на диск, без NCM.
- `StreamInstall` — только NSP/NSZ в установщик. Extra по умолчанию Skip.
- `PortInstall` — скачать выбранное на диск, затем `SwitchDeployService`
  копирует NRO в `/switch`, двигает LayeredFS в корень SD, ставит NSP
  форвардера **после** receipt.

Режим выбирается **не тем, что нажал пользователь**, а содержимым маски:

```
portInstall = selectedNro
           || (selectedZipOrLayered && selectedPackageCount == 0)
```

(`DownloadManager::importTorrentActions` / `importDebrid`,
`src/app/download_manager.cpp`).

Следствие: **NSP + zip перевода никогда не станет PortInstall**. Zip либо
Skip, либо Download в StreamInstall, после чего деплой его не понимает.

### 2.2 Классификаторы (три независимых, часто расходятся)

1. **Вкладка каталога** — `catalogEntryIsGame` /
   `catalogEntryIsPort` в `src/app/catalog_presentation.cpp`.
   Смотрит на **название релиза**: `[nro]` / `.nro` → порт; title id или
   `[nsp]` → игра. Содержимое торрента не читается.
2. **Разметка торрента** — `torrentPortLayoutDetected` в
   `src/app/port_selection.hpp`. Порт, если есть NRO **или** (нет NSP и есть
   zip/7z / LayeredFS). NSP + zip / NSP + LayeredFS → **не порт**.
3. **Деплой** — `inspectSwitchDeploy` в `src/app/switch_deploy.cpp`.
   Ищет директории, в которых лежит валидный NRO; отдельно планирует
   `atmosphere/contents/<16 hex>/romfs/…`; zip/7z пробует как **port archive**
   (только NRO-корни или legacy `switch/`).

One-tap с вкладки Games идёт в `GameDetailActivity::finishImport` и **не
вызывает** `defaultTransferMode` / `startPortInstall`. Даже если в торренте
есть NRO, маска — `selectSmartInstallFiles` (только NSP).

One-tap с вкладки Ports идёт в `startPortInstall` →
`selectPortInstallActions`.

### 2.3 Что считается «полезным extra», а что мусором

Код называет extra всё, что не NSP/NSZ и не XCI:

- readme / nfo
- zip/7z (русификатор, `switch.7z`, пак текстур)
- дерево LayeredFS
- любые папки вроде `Русская озвучка/`, `translation/`

Различия в коде **нет**. `selectionSkipsExtraFiles`
(`src/app/game_update_install.hpp`) считает extra любой non-package Skip
рядом с хотя бы одним Install — и one-tap показывает
`installing_extras_skipped`. Пользователю говорят «включите через Выбрать
файлы», но этот путь сам по себе **не ставит** перевод на SD.

### 2.4 Debrid vs прямой торрент

В `src/app/debrid_transfer.cpp`, `selected()`:

```
if (spec.mode == StreamInstall && !isPackageName(file.path))
    return false;
```

Даже если в пикере extra отмечен Download, **debrid его не качает**.
Прямой BitTorrent качает Download-файлы. Поведение расходится.

---

## 3. Матрица раскладок

`W` — работает end-to-end. `P` — частично (скачается или поставится не то).
`F` — ломается / молча теряется.

| Раскладка торрента | Каталог | One-tap | Пикер «как есть» | Деплой |
|---|---|---|---|---|
| `switch/Game/Game.nro` + data | Порт, если в названии NRO | W (PortInstall) | W | W → `/switch/Game/` |
| zip/7z с NRO внутри, без NSP | Порт (нет пакетов) | W | W | W, extract в `/switch` |
| NSP форвардер + NRO | Часто **Игра** (есть title id) | **F**: smart install ставит только NSP | P: надо руками выбрать NRO | W, если NRO скачан |
| Только LayeredFS romfs | Порт | W | W | W, move на SD |
| NSP + `atmosphere/contents/…/romfs` | Игра | **F**: LayeredFS Skip | P: P2P скачает, debrid нет; деплой может предложить copy | P, если файлы на диске и нет конфликта с уже стоящими модами |
| NSP + `rusifikator.zip` (внутри atmosphere, **без NRO**) | Игра | **F** Skip | **F**: не PortInstall; probe «no NRO payload»; zip мёртвый в downloads | F |
| NSP + `switch.7z` (настоящий порт в архиве) | Игра | **F** Skip | P: zip Download, режим StreamInstall, auto-deploy только если archive extractable | P |
| NRO в корне + `data/file.bin` (не multi-torrent) | Порт | **F**: `pathUnderPortRoot("")` берёт только файлы без `/` | F та же маска | data не копируется |
| `exefs/` патч (часто нужен языковым патчам) | — | Skip | Skip | **намеренно не деплоится** (`isLayeredFsRomfsPath`) |
| `.rar` | — | не архив порта | Download как other | F, rar не распаковывается |
| `contents/<tid>/…` без префикса `atmosphere/` | — | extra | extra | F, путь не распознан |
| Linux-образ / не-NRO «порт» | Порт | PortInstall качает всё | качает всё | `LayoutNotFound`, файлы остаются в downloads |
| Readme рядом с NSP | Игра | Skip, тост extras skipped | Skip | ок |

---

## 4. Пользовательские симптомы и откуда они

### 4.1 «Поставил игру — нет русского / нет озвучки»

One-tap (`GameDetailActivity::finishImport` /
`startDebridInstall`) → `selectSmartInstallFiles` → только NSP.
Тост `pipensx/detail/installing_extras_skipped`.

Файлы: `src/ui/detail/game_detail.hpp`, `src/app/game_update_install.cpp`,
`src/app/download_manager.hpp` (`defaultFileAction`).

### 4.2 «Выбрал все файлы — перевод всё равно не встал»

- **Debrid + StreamInstall**: extra отфильтрован в `debrid_transfer.cpp`.
- **P2P + StreamInstall**: extra качается в
  `/switch/pipensx/downloads/<name>-<hash8>/` и **никуда не копируется**,
  потому что `considerDeployOffer` для zip без NRO ставит `NotAPort`.
- Пикер повышает режим до PortInstall **только** при выбранном NRO или при
  zip/LayeredFS **без** пакетов (`selectedPortTransaction` в
  `src/ui/detail/torrent_selection.hpp` — LayeredFS при наличии NSP
  payload’ом не считается).

### 4.3 «Порт не запускается, в downloads пустая папка»

#6. Два независимых механизма:

1. StreamInstall **не пишет** NSP на SD как файл — он стримится в NCM.
   Папка задачи пустая, это норма для игр и выглядит как баг для портов.
2. Если релиз ошибочно пошёл как игра, NRO/data не качались. Деплой в
   `/switch` не предлагался (`LayoutNotFound` — «нормальный NSP-случай»).

После успешного PortInstall UI предлагает удалить downloads
(`src/main_switch.cpp`, `deploy/cleanup_question`). Если extra так и не
скопировался, пользователь стирает единственную копию.

### 4.4 «Не видно, какие файлы где лежат»

`TaskFilesActivity` показывает `logicalPath` и `absolutePath` в папке
задачи. **Путь назначения** (`/switch/…`, `/atmosphere/contents/…`) есть
только в плане деплоя и не попадает в карточку файла. Receipt после
деплоя в UI не разбирается.

Файлы: `src/ui/downloads/task_files_activity.hpp`,
`src/ui/downloads/details_activity.hpp`.

### 4.5 «Поставил перевод — pipensx отказал из‑за конфликта»

LayeredFS: любое **лишнее** файл в том же
`atmosphere/contents/<tid>/romfs/` (старый мод пользователя) →
`SwitchDeployProblem::Conflict`, установка блокируется целиком.
Смешивать версии/моды код отказывается специально
(`tests/test_switch_deploy.cpp`). Для русификатора поверх существующего
мода это выглядит как «перевод не ставится».

### 4.6 Каталог врёт про вкладку

Порт с title id оригинальной игры и без `[NRO]` в названии попадает в
Games. One-tap — smart install, не port dialog. Обратно: дамп с `.nro` в
названии, но по факту NSP+перевод, откроет port warning и
`selectPortInstallActions` (скачает zip, который потом, возможно, не
распакуется).

---

## 5. Разрывы в коде (где чинить)

Ниже — не «идеи», а расхождения между инвариантами. Приоритет: P0 ломает
основной сценарий, P1 — частый, P2 — крайние раскладки / UX.

### P0. Игра + deployable extra должна ставиться как двухфазная транзакция

**Проблема.** `torrentPortLayoutDetected` обнуляет порт при любом NSP.
`importTorrentActions` не поднимает PortInstall, если выбран zip/LayeredFS
вместе с пакетами. Debrid в StreamInstall extra не качает.
`probePortArchive` / `buildArchiveMapping` (`src/app/port_archive.cpp`)
отбрасывают архив без NRO и без сегмента `switch/` — типичный zip
русификатора (`atmosphere/contents/…`).

**Где:**

- `src/app/port_selection.hpp` — `torrentPortLayoutDetected`,
  `selectPortPayloadActions`, `selectPortInstallActions`
- `src/app/download_manager.cpp` — условие `portInstall`
- `src/ui/detail/torrent_selection.hpp` — `selectedPortTransaction`
  (zip/LayeredFS + NSP должны давать транзакцию с деплоем extra)
- `src/app/debrid_transfer.cpp` — `selected()` не должен выкидывать extra,
  если action = Download или режим — смешанный
- `src/app/port_archive.cpp` — `buildArchiveMapping`: третий корень —
  LayeredFS-члены архива → SD `atmosphere/contents/…`
- `src/app/switch_deploy.cpp` — планировать extract zip как LayeredFS, не
  только как NRO-порт
- `src/app/nx_file_types.hpp` — распознать LayeredFS внутри archive member
  path

**Что сделать:** ввести понятие **deployable extra** (LayeredFS-путь,
zip/7z, в котором probe нашёл romfs или NRO) vs **junk extra** (txt/nfo/
url). One-tap:

1. StreamInstall / commit NSP как сейчас.
2. Скачать только deployable extras.
3. Задеплоить их (move LayeredFS, extract zip).
4. Ошибка extra **не** переводит задачу в Error, если пакеты уже
   Installed (инвариант `343524f` сохранить).

Не поднимать весь релиз в PortInstall только из‑за zip: PortInstall
держит NSP на диске до конца, это дорого по месту. Нужна гибридная фаза
«пакеты стримом, extras деплоем».

### P0. One-tap с Games должен смотреть в торрент, не только в вкладку

**Проблема.** `portInstall_` берётся из секции каталога. `finishImport`
всегда `selectSmartInstallFiles`. Торрент с NRO+NSP с вкладки Games
теряет payload.

**Где:** `src/ui/detail/game_detail.hpp` (`startInstall`, `finishImport`,
`startDebridInstall`), `src/app/install_space.cpp` (`defaultTransferMode`
уже умеет upgrade — UI его не вызывает на one-tap игры).

**Что сделать:** после preview, если `torrentPortLayoutDetected` или есть
deployable extras — не smart-only. Либо port dialog, либо гибрид из P0
выше. Batch (`src/app/catalog_batch_installer.cpp`) использует
`defaultTransferMode` — игры с extras в batch тоже Skip, тот же фикс.

### P0. Кнопка «Выбрать файлы порта» пропускает форвардер NSP

`TorrentSelectionDataSource::selectPortFiles` зовёт
`selectPortPayloadActions`, не `selectPortInstallActions`. Форвардер
остаётся Skip. Порт копируется в `/switch` без иконки в меню.

**Где:** `src/ui/detail/torrent_selection.hpp`.

### P1. Плоский NRO + подпапка data не выбирается

`pathUnderPortRoot` при пустом root: только файлы без `/`.
`buildArchiveMapping` при пустом root наоборот берёт **весь** архив.
Селекция и extract расходятся.

**Где:** `src/app/port_selection.hpp` (`pathUnderPortRoot`,
`candidatePortPayloadRoots`), `src/app/port_archive.cpp`
(`pathInside` vs селекция), тесты в `tests/test_switch_deploy.cpp`
(добавить кейс `Game.nro` + `data/x.bin` при `multi=false`).

**Что сделать:** пустой root = «весь торрент / весь архив, кроме junk»,
одинаково в обоих местах. Либо запретить empty root и всегда брать
parent NRO, для root-level NRO — синтетический корень имени релиза.

### P1. Zip перевода ≠ port archive

`isPortArchiveName` = любой `.zip`/`.7z`. Probe требует NRO или `switch/`.
Русификатор почти никогда не содержит NRO.

**Где:** `src/app/nx_file_types.hpp`, `src/app/port_archive.cpp`,
планировщик архивов в `inspectSwitchDeploy` (~строки 1255–1332).

**Что сделать:** probe двух сортов: `PortNroArchive` и `LayeredFsArchive`.
Второй пишет member path с offset `atmosphere/` на корень SD (как loose
LayeredFS). Без NRO это не `NotAPort`, а extras-deploy.

RAR (`isRarName`) — отдельный P2: формат есть в классификаторе, extract
нет.

### P1. Debrid и P2P по-разному исполняют одну маску

Одна и та же `fileSelection` с Download на zip:

- P2P качает zip;
- debrid делает вид, что файла нет.

**Где:** `src/app/debrid_transfer.cpp` (`selected`, цикл после
`streamInstallPackage` с `continue` на StreamInstall).

Согласовать с гибридным режимом: Download extra качается после пакетов,
фейл extra не валит Installed.

### P1. Список файлов не показывает назначение на SD

#6, вторая половина.

**Где:** `src/ui/downloads/task_files_activity.hpp` (`showFile`,
`TaskFileCell::setFile`), план в `SwitchDeployEntry.destinationPath`.

Показать для каждого файла: роль (package / nro / layeredfs / junk),
действие, путь на SD после деплоя или «останется в downloads».

### P1. Конфликт LayeredFS слишком грубый для переводов

Любой чужой файл в том же romfs-tree → Conflict на весь план. Русификатор
не встанет поверх текстур-мода, и наоборот.

**Где:** `src/app/switch_deploy.cpp` (скан `layeredRomfsRoots` vs
`layeredExpected`; conflict на copy NRO тоже SHA-256 всего файла).

**Что сделать для переводов:** overlap по **именам файлов релиза** —
перезаписывать совпавшие, чужие не трогать (как uninstall уже не трогает
`user-mod.rpf`). Предупреждение, не полный стоп. Для NRO-портов полный
Conflict оставить: перезапись чужого `.nro` опаснее.

### P2. Каталог Ports vs Games не смотрит в info dict

`catalogEntryIsGame` по title string. Релизы с borrowed title id без
`[NRO]` — не на той вкладке, не тот one-tap.

**Где:** `src/app/catalog_presentation.cpp`. Долгосрочно: флаг из
генератора каталога / info-dict (`has nro`, `has layeredfs`). Пока — после
resolve переклассифицировать кнопку Install на карточке.

### P2. exefs / неканоничные корни переводов

`isLayeredFsRomfsPath` требует ровно
`[prefix/]atmosphere/contents/<16hex>/romfs/<member>` и **исключает
exefs**. Реальные паки:

- `atmosphere/contents/<tid>/exefs/` (патч кода под язык)
- `contents/<tid>/romfs/` без `atmosphere/`
- `titles/<tid>/` (старый layout)
- SaltyNX / просто папка `Русский/`

Сейчас всё это junk. Расширять осторожно: exefs — сознательно вырезан
(«больший security surface»). Если делать — отдельный opt-in в пикере
«исполняемые патчи», не one-tap.

### P2. Мусорные пути и 0-байт файлы

Деплой уже скипает не-NRO без источника (`.cosmo`, 0-byte). Архивы с
коллизией имён на FAT падают целиком (`UnsafePath`). Крупный 7z solid
блок: `portArchiveSolidFitsRam` + `NoRam`. Это правильные предохранители;
в UI `problem_ram` / `problem_unsafe` нужно человеческим языком и советом
«выберите подмножество / распакуйте на ПК».

### P2. `AmbiguousLayout` мёртв

Enum есть, `inspectSwitchDeploy` его никогда не ставит (мульти-NRO корни
теперь легальны). Либо убрать, либо вернуть для двух несвязанных title id
в одном LayeredFS-торренте без NSP.

### P2. UX тоста extras

`installing_extras_skipped_hint` обещает, что «Выбрать файлы» включит
перевод. Это ложь, пока P0 не сделан. Пока гибрида нет — текст должен
говорить: extra останется в downloads и **не поставится в игру**.

Ключи: `resources/i18n/*/pipensx.json`
(`installing_extras_skipped`, `_hint`). После смены прогнать
`scripts/check_i18n.py`.

---

## 6. Предлагаемый инвариант (чтобы не разъехалось снова)

Зафиксировать в коде и тестах, не в комментариях:

1. **Package commit успешен ⇒ задача не Error** из‑за extra. Уже есть для
   torrenting-off и debrid leftover; распространить на probe/extract extra.
2. **Deployable extra, выбранный пользователем или one-tap, должен либо
   оказаться на SD по каноническому пути, либо дать явную ошибку extra**,
   не «Installed» с пустым `/atmosphere`.
3. **Junk extra никогда не валит установку и не занимает режим.**
4. **Одна маска файлов = одно поведение** на torrent и debrid.
5. **Селекция и extract** одинаково понимают «корень приложения».
6. **Каталожная вкладка не выбирает TransferMode.** Режим — из preview
   торрента.

Типы extra (черновик, одно место — `nx_file_types.hpp` + маленький
`extra_kind.hpp`):

- `Package` — nsp/nsz
- `Cartridge` — xci/xcz (как сейчас: не ставим)
- `NroPayload` — nro + файлы под тем же parent
- `LayeredFsRomfs` — канонический atmosphere romfs
- `PortArchive` — zip/7z с NRO
- `LayeredFsArchive` — zip/7z с atmosphere romfs, без NRO
- `Junk` — остальное

---

## 7. Тесты, которых не хватает

Сейчас зелёные тесты **закрепляют баг** как контракт:

- `tests/test_install_space.cpp` — NSP + `switch.7z` → StreamInstall, zip
  Skip
- `tests/test_switch_deploy.cpp` — NSP + zip `!torrentPortLayoutDetected`
- `tests/test_manager.cpp` — debrid «только форвардер» не подмешивает NRO
  (это правильно); нет кейса «форвардер + NRO с Games one-tap»

Добавить (имена ориентир):

- `test_smart_install_skips_junk_but_keeps_layeredfs` — после фикса P0
- `test_nsp_plus_rusifikator_zip_deploys_atmosphere` — zip без NRO,
  probe LayeredFsArchive, файлы на SD
- `test_debrid_stream_install_downloads_selected_extras`
- `test_flat_nro_selects_data_subdir` — `multi=false`
- `test_select_port_files_includes_forwarder_nsp`
- `test_game_detail_preview_upgrades_mode_when_nro_present` (хотя бы на
  уровне `defaultTransferMode` + smart mask helper, без UI)
- `test_layeredfs_conflict_overlaps_only` — чужой `user-mod` не блокирует
  русификатор (если примем P1)
- `test_extra_failure_after_packages_stays_installed` — уже частично есть
  для torrenting-off; повторить для extract fail

Гнать `make -f Makefile.pc test` и `make switch`: деплой/libnx трогает
оба мира.

---

## 8. Порядок работ (если чинить)

Не делать всё сразу. Зависимости:

1. **Классификатор extra** (`nx_file_types` / новый helper) + тесты на
   имена. Без смены поведения.
2. **LayeredFsArchive probe** в `port_archive.cpp` + планировщик в
   `switch_deploy.cpp`. Можно вручную через пикер (Download zip → деплой).
3. **Выровнять debrid `selected()`** с маской Download.
4. **Гибридный one-tap**: smart packages + deployable extras; сохранить
   «extra fail ≠ Error».
5. **Пикер / кнопка Select port** + плоский NRO root.
6. **UI путей назначения** и честный тост.
7. Каталожная переклассификация и exefs — отдельно, после того как
   канонический atmosphere-перевод встаёт one-tap’ом.

Issue #6 закрыт комментарием «partially made in 1.3.1». По коду основная
дыра (игра + файлы не для `/switch`, и zip без NRO) **не закрыта**. Имеет
смысл завести новый issue на гибридный extra-deploy и не расшивать #6
задним числом.

---

## 9. Чего не надо чинить

- Пропуск **junk** (readme) на one-tap — правильно, место и время.
- Отказ ставить XCI/XCZ — отдельный продукт, не extra.
- Не деплоить exefs молча — оставить, пока нет явного согласия.
- Mods tab (#55) — pipensx не браузер модов; перевод в релизе игры это
  extra релиза, не ModCD.
- Удаление downloads после успешного деплоя — ок, если extra уже на SD
  (сейчас диалог опасен именно потому, что extra часто ещё в downloads).
