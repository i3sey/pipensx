# План: eShop-иконки через game_metadata_index.json (без NLib)

Цель — вернуть красивые квадратные eShop-иконки и ENG-описание в каталог
pipensx, не задерживая актуальные записи Langegen. `game_metadata_index.json`
строится отдельным GitHub Actions-пайплайном и обновляется приложением в
runtime; romfs остаётся необязательным offline-фолбэком. **Без NLib.**

Статус 2026-07-09: runtime-клиент и генератор реализованы; отдельный локальный
репозиторий подготовлен в `/home/sa05/pipensx-metadata`.

## Что показал гит и код

- "Раньше прекрасно работало" = индекс **бандлился** через
  `make switch PIPENSX_METADATA_INDEX=/abs/game_metadata_index.json`
  (`BUILD.md`, `CMakeLists.txt:189`). Public build намеренно без него
  (`game_metadata_service.cpp:447 load()` → `byHash_` пуст → grid плитки
  без иконок).
- Статический механизм был цел, но для независимых обновлений потребовались
  runtime manifest, SHA-256-проверка и content-addressed кэш.
- `refreshDetails()` (NLib-рантайм, `game_metadata_service.cpp:482`)
  **объявлен, но нигде не вызывается** (`grep -r refreshDetails src/` → только
  объявление). NLib-путь мёртв. Исключить NLib из плана = ничего не теряем.

## Механизм приложения

- `CMakeLists.txt:189-201` — `PIPENSX_METADATA_INDEX` копируется в
  `romfs:/catalog/game_metadata_index.json`. `resources/catalog/` в
  `.gitignore` (датасет не коммитится).
- `GameMetadataService(rootPath, bundledPath="romfs:/catalog/game_metadata_index.json")`
  (`game_metadata_service.hpp:45`).
- `load()` предпочитает проверенный runtime-кэш, затем romfs; `fetchLatest()`
  скачивает manifest/index на worker-потоке, а `adopt()` меняет активный map
  только на UI-потоке.
- `load()` (`cpp:447`) — читает индекс (лимит `kMaxIndexBytes = 24 МБ`,
  `cpp:32`), парсит через `parseIndex` в `byHash_` (ключ = `infoHash`,
  40 hex UPPER). Нет файла → `byHash_` пуст, return ok.
- `parseIndex` (`cpp:415`) + `parseMetadataObject` (`cpp:339`) — поля:
  `infoHash`(обязат, 40 hex), `titleId`/`id`, `name`(обязат), `match`,
  `intro`, `description`, `publisher`, `releaseDate`, `iconUrl`/`icon`,
  `bannerUrl`/`banner`, `screenshots[]`/`screens[]` (≤4), `categories[]`/
  `category[]` (≤6). Запись без `titleId` или `name` пропускается.
- Сетка: `catalog_view.hpp:692 findByInfoHash(entry.infoHash)` →
  `:697 iconUrls.push_back(meta ? meta->iconUrl : "")`. **Grid artwork =
  только `meta->iconUrl`**; без индекса — пустой placeholder (не rutracker
  poster). Hero-карта фолбэчит на `posterUrl` (`:751`).
- Детальная карточка: `game_detail.hpp:147` — `coverUrl = found->iconUrl ||
  found->bannerUrl || entry_.posterUrl`. Без индекса → rutracker cover.
- Релеи eShop CDN (`game_metadata_service.cpp:183-260`, `#ifdef __SWITCH__`):
  `isNintendoImageUrl()` = префикс `https://img-eshop.cdn.nintendo.net/`.
  Этот CDN **гео-блокнут в РФ** → фан-аут по 4 релеям, первый 200 побеждает:
  `i0.wp.com` (Photon), `external-content.duckduckgo.com/iu/`,
  `images.weserv.nl` (http и https). На PC релеев нет (CDN прямой, вне РФ
  доступен).

→ Вывод: чтобы вернуть иконки, надо **сгенерировать и забандлить индекс**.
Парсер, relay, UI — всё готово. Менять код pipensx не требуется.

## Источник: blawar/titledb (без NLib)

`https://github.com/blawar/titledb` — публичный per-region датасет eShop,
обновлён 2026-07-06 (актуален). Файлы вида `<REGION>.<lang>.json`
(`US.en.json` 84 МБ, `RU.ru.json` 51 МБ, `JP.ja.json` 83 МБ, …). Ключ — nsuId.

Запись (проверено, US.en.json):
```
id            01007EF00011E000          # eShop Title ID (16 hex) — КЛЮЧ матчинга
name          The Legend of Zelda™: …
intro         STEP INTO A WORLD OF …
description   Forget everything you know … # ENG описание
publisher     Nintendo
releaseDate   20170303                  # YYYYMMDD
iconUrl       https://img-eshop.cdn.nintendo.net/i/<64hex>.jpg   # ← 1024×1024
bannerUrl      "            "            # ← hero-баннер
screenshots[]   "            "            # ← eShop скриншоты
category[]    ['Adventure','Action','RPG']
languages[]   ['ja','en',…,'ru']
isDemo        false                     # фильтровать true
size          15479078912
```

- iconUrl/bannerUrl/screenshots = `img-eshop.cdn.nintendo.net/i/<64hex>.jpg`.
  **Живой**: GET range → HTTP 206, `image/jpeg`, 1024×1024, Exif «Nintendo
  AuthoringTool». Через `images.weserv.nl`-relay (путь Switch) → тоже 200.
  Префикс точно совпадает с `isNintendoImageUrl()` — relay сработает.
- Base application определяется по обнулённым младшим 12 битам Title ID, то
  есть ID заканчивается на `000`, но не обязательно на `0000`. Для иконки/base
  достаточно base-app id; DLC/update делят иконку base.
- Дает **ENG name/intro/description**, eShop скриншоты (могут заменить
  rutracker-thumb), category для жанровых полок (`catalog_view.hpp` фильтр по
  `meta->categories`).
- RU-описание/имя — из `RU.ru.json` при желании (отдельный регион-файл).
  ENG по умолчанию (соответствует «ENG description» в цели).

## Схема выходного индекса (ровно под `parseMetadataObject`)

Фикстура-референс: `tests/fixtures/golden/game_metadata_index.json`.

```json
[
  {
    "infoHash": "AA7089C9CF3161C609672408CF34C5B2114180A3",  // UPPER 40 hex — из switch_games.json magnet
    "titleId":  "01003A30012C0000",                          // titledb id
    "match":    "LEGO Ninjago Shadow of Ronin",               // rutracker нормализованный title (для отладки)
    "name":     "LEGO Ninjago: Shadow of Ronin",              // titledb name
    "intro":    "...",
    "description": "...",                                    // titledb (ENG)
    "publisher": "Warner Bros",
    "releaseDate": "20150327",
    "iconUrl":  "https://img-eshop.cdn.nintendo.net/i/<hex>.jpg",
    "bannerUrl":"https://img-eshop.cdn.nintendo.net/i/<hex>.jpg",
    "screenshots": ["https://img-eshop.cdn.nintendo.net/i/<hex>.jpg", "..."],
    "categories": ["Action","Adventure"]
  }
]
```
- Только matched записи (infoHash ← rutracker, поля ← titledb).
- `screenshots` ≤4 (парсер кап). `categories` ≤6.
- Записи без `titleId`/`name` парсер всё равно выкинет → не класть.

## Пайплайн генерации (отдельный репозиторий)

```
repo: i3sey/pipensx-metadata
.github/workflows/build-index.yml  # cron + dispatch
build_index.py   # fetch titledb + match + emit game_metadata_index.json
```

Шаги:
1. `git clone --depth 1 https://github.com/blawar/titledb` (или raw-fetch один
   регион-файл; US.en.json 84 МБ — один файл за раз, в Actions укладывается).
   Фильтр: `id` 16 hex и `(id & 0xFFF) == 0`, `isDemo == false`,
   есть `iconUrl` и `name`. Срезать дубли name→любой регион.
2. Вход rutracker-стороны: `switch_games.json` (форк/локальный) → список
   `{infoHash(UPPER), title, topic_id}`. `infoHash` = btih из magnet UPPER.
3. Матчинг title → titledb `id` (см. ниже).
4. Эмит `game_metadata_index.json`: merged запись (схема выше). Сортировка
   произвольна (`byHash_` map, не порядок).
5. Валидация: JSON array, `0 < len ≤ 20000`, байты ≤ `kMaxIndexBytes` (24 МБ),
   каждая запись: `infoHash` 40 hex UPPER + парсится `titleId` + непустой
   `name` + `iconUrl` начинается с `img-eshop.cdn.nintendo.net/`. Провал →
   не публиковать, алерт.
6. Публикация: immutable GitHub Release с индексом, manifest и отчётом.
   Клиент проверяет исходный URL, конечный GitHub asset redirect, размер,
   SHA-256 и число записей.
7. Размер: при 4000 записей × ~3 КБ ≈ 12 МБ < 24 МБ. Если упирается — трим
   `description` ≤1500, `screenshots` ≤3. Без изменения лимита приложения.

## Матчинг rutracker → Title ID (главное, куда копать)

Жёстких данных достаточно — точный матч 63% «из коробки»:
нормализация (`strip [..]`, `strip (..)`, lowercase, punct→space, collapse
spaces) → 2572/4082 совпадают с titledb `name` 1-в-1. Копать:

1. **Exact normalized** (база, 63%): нормализовать оба title, dict-match.
2. **DLC/бандлы** (`Mortal Kombat 11 + 31 DLC`, `GigaBash + 8 DLC`): отрезать
   `+ N DLC` и хвост, матчить базовый title. Копать регекс `^(.*?)(\s*\+\s*\d+\s*DLC)?`.
3. **Multi-паки** (`Olli Olli + OlliOlli 2…`, `Anodyne / Anodyne 2`): сплит по
   ` / ` и ` + `, матчить каждый, брать первый найденный.
4. **Транслит/русские имена** (`Пинбол «Звёздный юнга»`): копать
   транслитерацию CYR→LAT + алиасы; приоритет — матч по English-имени из
   rutracker-описания (поле `description` иногда начинается с латинского
   названия). Также `RU.ru.json` titledb содержит русские имена eShop —
   матч ru↔ru.
5. **Title ID прямо в контенте** (точный матч, без fuzzy): локальный
   rutracker XML-дамп `~/pipensx-local-data/rutracker-20260530.xml.xz` (4.1 ГБ)
   в `content` BBCode иногда содержит Title ID (`0100...`). Копать: извлечь
   регексом `\b0100[0-9A-F]{9}000\b` из `content` темы → точный `topic_id
   → titleId`. Это лучший сигнал, если есть.
6. **Homebrew/порты** (`Hurrican (хоумбрю Turrican)`, `Dolphon эмулятор`,
   NRO-релизы): нет eShop-записи → нет иконки. Оставить без индекса → grid
   Langegen `cover`; grid/detail используют field-by-field fallback.
7. **Дедуп/регионы**: titledb дублирует игру по регионам (US/JP/EU). Брать
   US.en как primary (ENG, 1024² иконка), EU/JP — если US нет. Один titleId
   на infoHash.
8. **Метрика**: логировать override/title-id/exact/transformed/unmatched и
   отдельные fuzzy-предложения на каждом прогоне; fuzzy не публикуется
   автоматически. Алерт при падении покрытия более чем на 2 п.п.

## Встраивание в pipensx

- Refresh Langegen и metadata запускаются параллельно и принимаются независимо:
  свежий каталог не блокируется ошибкой индекса, старый валидный индекс не
  теряется при ошибке каталога.
- Manifest проверяется до активации, индекс хранится по SHA-256, активный
  manifest заменяется последним атомарным rename.
- Grid: `eShop icon → Langegen cover → placeholder`; detail/hero/описание и
  факты используют тот же единый resolver.
- График генератора: каждые 6 часов + dispatch, публикация только при изменении
  SHA Langegen или titledb.

## Что план НЕ делает

- **NLib** — исключён (мёртвый путь в коде; не нужен при titledb).
- **Скриншоты rutracker `thumb`→`big`** — отдельный план
  (`CATALOG_SCREENSHOTS_PLAN.md`). Индекс может нести eShop-скриншоты как
  альтернативу, но grid/деталь используют `meta->screenshots` только на
  детальной карточке (`catalog_presentation.cpp:20`), не в grid — копать
  отдельно.
- **Image mirror** — не создаётся; остаются Nintendo CDN URL, relay-фан-аут и
  существующий disk/memory cache.

## Куда копать — чек-лист

- [x] Исправить base-app фильтр: младшие 12 бит равны нулю (`...000`).
- [x] Реализовать deterministic matching, overrides, manifest и regression gate.
- [x] Реализовать runtime cache/adopt и Langegen fallback в grid/detail.
- [ ] Тест relay-фан-аута на реальном Switch в РФ: какой relay отвечает
      первым для `img-eshop.cdn.nintendo.net` (лог `[metadata] image relay
      active`).
- [x] Live-срез: 2954/4108 deterministic matches (71.9%), индекс 6.8 МБ.
- [ ] Создать и запушить GitHub-репозиторий `i3sey/pipensx-metadata`, затем
      выполнить первый workflow publish.
