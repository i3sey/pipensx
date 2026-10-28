# Live seeding в pipensx: раздача во время загрузки и установки

Дата: 2026-10-21
Ревизия: 5 (implementation-ready: аудит перепроверен по коду, план разбит на
фазы с файлами, API, тестами и критериями готовности).
Режим: утверждённая целевая архитектура, план реализации.
Область: Direct BitTorrent, задачи `StreamInstall` и `DownloadOnly`.

> **Главный фокус:** раздача кусков другим пирам прямо в процессе загрузки и
> установки игры в NCM (StreamInstall Live Seeding) — ядро функционала.
>
> **Ключевые решения (без изменений с ревизии 4, уточнены в ревизии 5):**
> 1. **Изолированный Ring-пул.** Отказ от перехвата ownership у `job->buf`
>    (`piece.c`). Пул `PIECE_BUF_POOL_MAX = 4` не трогаем; Ring-кэш владеет
>    своими буферами и делает быстрый `memcpy` на torrent-потоке
>    (оверхед CPU < 0.2% на Switch).
> 2. **Битфилд отдачи = существующий `available_bf`** (открытие ревизии 5):
>    движок уже различает «скачано» (`have_bf`) и «можно отдать»
>    (`available_bf`, piece.h:60, шлётся в хэндшейке как `ctx->our_bf`,
>    torrent.c:802). Ring-кэш просто ставит/снимает биты в нём. Параллельный
>    `seed_bf` из ревизии 4 не нужен.
> 3. **Интеграция с `StreamBudgetArbiter`.** Размер кэша вписывается в общий
>    RAM-бюджет; в Applet Mode кэш ужимается или отключается.
> 4. **Неблокирующий Install Drain.** `finishBegin()` / `finishPoll()` вместо
>    блокирующего `PackageCoordinator::finish()`: пока NCM дописывает пакеты,
>    `torrent_tick()` продолжает отдавать.
> 5. **Разделение сценариев.** `StreamInstall` сидирует из RAM-окна; после
>    инсталла сид остаётся только если игра целиком влезла в кэш, иначе честный
>    `Installed` с освобождением памяти. `DownloadOnly` после финиша сидирует
>    с SD без ограничений объёма.
> 6. **Протокольная честность.** Реальные `uploaded`/`downloaded` в HTTP и UDP
>    анонсах (UDP — поле на offset 72, BEP-15); BEP-27 `private=1` гасит
>    DHT/PEX; BEP-6 `REJECT_REQUEST` для вытесненных кусков.
> 7. **Батарея.** В чистом `Seeding` авто-сон консоли не блокируется.

---

## 0. Короткий вывод

1. **Сейчас pipensx — чистый лич.** Входящие `MSG_REQUEST`/`MSG_CANCEL` не
   обрабатываются (`peer.c`, `process_message`), `MSG_UNCHOKE` не отправляется,
   `peer_choked` навсегда `1` (peer.c:128–129), `uploaded` трекеру шлётся как
   `0`, счётчиков отдачи в коде нет вообще. Отдачи как пути кода нет.
2. **Live-сид делается на том же engine.** Не нужен второй торрент и не нужно
   ждать `finish()`: принять `REQUEST`, валидировать, отдать `PIECE` из
   Ring-кэша (или с SD для `DownloadOnly`), публиковать готовые куски в
   `available_bf`.
3. **Live-сид начинается с первого проверенного куска.** Кусок проверен и
   скопирован в Ring → бит в `available_bf` → `HAVE` существующим пирам,
   актуальный bitfield новым.
4. **Финальный install drain не блокирует отдачу:** пока инсталлер дописывает
   NCA в NCM, `torrent_tick()` работает и отдаёт из Ring.
5. **Кэш — скользящее окно в Ring-пуле** (256/512/1024 МБ по бюджету). Запрос
   вытесненного куска → BEP-6 `REJECT_REQUEST` (или choke/unchoke для старых
   пиров). Отставшие пиры ротируются.
6. **Раздельные правила завершения:** см. решение 5 выше и матрицу 3.2.
7. **Потокобезопасность «из коробки»:** запись и чтение Ring происходят только
   внутри `torrent_tick()` (torrent-поток). Мьютексы на горячем пути не нужны.
8. **До сидирования чинится трекерная честность:** реальные `uploaded` (его
   сначала надо начать считать) и `downloaded` (сейчас уходит
   `completed_bytes`, а не байты сессии — torrent.c:1637), BEP-27 приватность.
9. **Сон и батарея защищены:** `Seeding` исключён из `hasActiveTransfer()`.

---

## 1. Требования и границы

### 1.1. Что требуется

1. **Раздавать во время загрузки и установки.** Проверенный кусок мгновенно
   доступен рою, включая фазу финального drain.
2. **StreamInstall — приоритет №1.**
3. **Честная статистика и трекер.** Реальные `uploaded`, `downloaded`, `left`.
   `left=0` и `event=completed` — только при реальном 100% наличии (для
   `StreamInstall` — после подтверждённого NCM-коммита).
4. **Не ломать установку.** Установка имеет абсолютный приоритет по памяти и
   CPU; при конкуренции отдача уступает.
5. **Безопасность памяти Switch.** Собственный Ring-пул, без воровства буферов
   у `piece_mgr`, без фрагментации кучи.
6. **Интеграция с RAM-арбитром.** Размер кэша согласуется с
   `StreamBudgetArbiter` (Applet vs Title Override).
7. **Протокольная совместимость.** BEP-3 `REQUEST`/`CANCEL`, BEP-6
   `REJECT_REQUEST`, BEP-21 `upload_only`, BEP-15 `uploaded` на offset 72.

### 1.2. Не цели v1

- Фоновый демон/sysmodule-сидирование (сид живёт, пока открыт pipensx);
- Super-seeding (BEP-16);
- MSE inbound responder (исходящий MSE работает; inbound MSE-пир получит
  отказ и переподключится плейнтекстом);
- Приём входящих μTP-соединений (сейчас `torrent.c:1078` отвергает их;
  входящий TCP принимается — torrent.c:906–911). Раздача достигает пиров,
  к которым мы подключились сами (TCP и μTP), плюс входящих TCP-пиров;
- Сложные анчок-алгоритмы (tit-for-tat-ротация, optimistic unchoke по
  скоростям) — v1 анчокает всех interested в пределах лимита слотов;
- Несколько параллельных `Seeding` задач после финиша (максимум 1 активный
  seed-слот);
- `event=stopped` на все трекеры (сейчас только `mi.trackers[0]`,
  `announce_stopped_async`, torrent.c:1699 — сохраняем поведение, добавляем
  `uploaded`).

### 1.3. Инженерные ограничения репозитория (обязательны для каждой фазы)

- Общий `src/core` и `src/app` компилируются как C++17 (CMake Switch — C++20).
- Тесты: `tests/test_*.c|cpp`, plain `assert`, регистрация в `Makefile.pc`
  (не в CMake). `test_manager` всегда последний в наборе.
- Изменение персистентных настроек (`app_settings.cpp`) или версионирование
  download-state (`download_manager.cpp` saveLocked/load) — загрузить скилл
  `debrid` перед работой. Фазы 4–5 его задевают.
- Новые строки локализации: сначала `en-US`, затем `ru`, `pt-BR`, `fr`;
  валидация `scripts/check_i18n.py`.
- Каждая фаза завершается сборочной матрицей: `make -f Makefile.pc test`,
  `make pc`, `make switch` (+ `check_i18n.py` для фаз с UI-строками).
  Правка только одного билда недостаточна.

---

## 2. Аудит кода (перепроверен по текущему main)

### 2.1. Сетевой тракт: факты

| Что | Где | Факт |
|---|---|---|
| Входящие `REQUEST`/`CANCEL` | `src/core/peer.c`, `process_message` (~:447–557) | В `switch (id)` нет веток `MSG_REQUEST` и `MSG_CANCEL` |
| `MSG_UNCHOKE` отправка | `peer.c` | Константа есть (peer.h:25), отправки нет |
| `peer_choked` | `peer.c:128–129` | Инициализируется `1`, никогда не сбрасывается |
| `upload_only` (BEP-21) | `peer.c:198` | В ext-хэндшейке жёстко `upload_onlyi0e` |
| `interested` | `peer.c:629` | Шлётся безусловно после bitfield, даже когда качать нечего — надо честно слать not-interested в seed-режиме |
| Счётчики upload | `src/core/*` | **Отсутствуют.** `peer_t` считает только `downloaded`; в `torrent_t` есть `t->downloaded` (байты сессии), но нет `t->uploaded`. Вводятся фазой 1 |
| Входящие TCP | `torrent.c:906–911` | Принимаются (`listen_fd` + `net_accept`) |
| Входящие μTP | `torrent.c:1078` | Отвергаются («outgoing-only») |
| `uploaded` HTTP | `tracker.c:89` (`http_build_announce_url`) | Захардкожен `&uploaded=0` |
| `uploaded` UDP | `tracker.c:261–272` (`udp_build_announce_packet`) | Байты 72–79 остаются нулями из `memset` — поле не пишется |
| `downloaded` announce | `torrent.c:1637` (`torrent_announce_totals`) | Уходит `pm->completed_bytes`, а не `t->downloaded` (байты сессии) |
| `stopped` | `torrent.c:1699` (`announce_stopped_async`) | Только первый трекер, без `uploaded`, detached-поток |
| `completed` | `torrent.c:2098` | Автоотправка при `num_done == num_pieces` — для `StreamInstall` это до NCM-коммита; надо уметь подавлять и слать после drain |

### 2.2. BEP-15 (UDP Tracker) — карта пакета

Announce Request — 98 байт, поле `uploaded` — int64 на **offset 72**:

- `56..63`: `downloaded` (int64)
- `64..71`: `left` (int64)
- `72..79`: `uploaded` (int64)  ← не пишется, ошибка
- `80..83`: `event` (int32)

Текущая реализация пишет `event` на 80 и оставляет 72–79 нулями. Исправление —
фаза 0.

### 2.3. Пул буферов `PIECE_BUF_POOL_MAX = 4`

- Пул определён в `src/core/piece.h:13`; возврат буфера в пул —
  `piece_buf_put()` в `hash_apply`-пути (`piece.c:217`).
- Если Ring заберёт ownership у `job->buf`, пул опустеет за 4 куска, и дальше
  каждый кусок — `malloc(piece_length)`/`free` → фрагментация кучи на Switch.
- **Решение:** Ring владеет своим массивом буферов; в момент успешного хэша —
  один `memcpy(ring_slot, job->buf, plen)`. Копирование 4 МиБ на Switch
  ~0.6 мс; при 10 МБ/с кусок приходит раз в ~400 мс → оверхед CPU < 0.2%.
- **Важный нюанс (ревизия 5):** существующий `hash_result_cb`
  (piece.h:105) вызывается в `piece.c` *после* `piece_buf_put()` — буфер уже
  возвращён в пул. Копирование в Ring должно происходить по новому хуку
  *до* возврата буфера (см. 5.2), иначе читаем память, которой уже владеет пул.

### 2.4. SINK-файлы и `check_completion`

- В `StreamInstall` пакеты — `STORAGE_FILE_SINK`; `storage_read()` для них
  всегда `-1` (`src/platform/storage.c:718`).
- Дисковая верификация `check_completion()` / повторный хэш для SINK-кусочков
  невозможны — данные уже распакованы в NCM.
- **Решение:** финализация `StreamInstall` управляется статусом
  `finishPoll()` координатора, а не дисковой верификацией (см. фазу 3).

### 2.5. Веб-сиды

- `torrent_submit_web_piece()` (torrent.c:577) режет веб-кусок на 16-КиБ
  блоки и передаёт их в штатный `piece_mgr_got_block()` → hash worker →
  `hash_apply`-путь. Куски веб-сидов попадают в Ring автоматически, отдельного
  кода не нужно.

### 2.6. Что уже есть и переиспользуем (не изобретаем заново)

| Примитив | Где | Как используем |
|---|---|---|
| `available_bf` — «pieces retained for upload» | `piece.h:60`, ставится в `mark_piece_done` только если `storage_range_readable()` (piece.c:143–146), снимается в `reset_piece` (piece.c:126) | Это и есть битфилд отдачи. Ring-кэш ставит/снимает биты в нём; `DownloadOnly` получает его бесплатно (SINK-нет → всё readable) |
| `ctx->our_bf = pm->available_bf` | torrent.c:802 | Хэндшейк-битфилд уже честный: для `StreamInstall` сегодня он пуст (никакой Ghost Seeding), с Ring — окно кэша |
| `broadcast_have()` | torrent.c:888 | Гейтить вызовом только когда бит реально встал в `available_bf` (кусок попал в Ring) |
| `t->downloaded` | torrent.c | Байты сессии для announce `downloaded` (замена `completed_bytes`) |
| `net_send` — неблокирующая отправка «сколько сокет примет» | `src/core/net.c` | Отправка `PIECE` из Ring/SD при `POLLOUT` без упаковки в `sbuf` |
| Входящий TCP-листенёр | torrent.c:906–911, `net_accept` | Пиры могут сами подключаться к сиду |
| `StreamBudgetArbiter` + `StreamRamBudget` | `src/app/stream_budget_arbiter.hpp` | Канал для бюджета Ring-кэша и resize-callback `applyBudget` |
| `RequestGate` / `adaptiveLookahead` | `src/app/request_gate.hpp`, `package_coordinator.hpp` | Сигнал «инсталлер занят» → буст upload-лимита (4.4) |
| `hasActiveTransfer()` | `download_manager.cpp:1847` | Точка исключения `Seeding` из блокировки сна |

### 2.7. Отсутствующие примитивы (создаются этим планом)

1. `peer_t->uploaded` и агрегат `torrent_t->uploaded` (байты отдачи сессии).
2. Очередь отдачи на пира (`up_queue`: index/begin/length, позиция отправки)
   и обработчики `MSG_REQUEST`/`MSG_CANCEL`/`MSG_REJECT_REQUEST`.
3. `peer_send_unchoke()` + честный `not-interested` в seed-режиме.
4. Модуль Ring-кэша (`src/core/seed_cache.{h,c}`) и хук копирования в
   `piece.c` до возврата буфера в пул.
5. `PackageCoordinator::finishBegin()/finishPoll()` + `DrainState`.
6. Публичный API `torrent_request_tracker_event(t, event)` (сейчас
   `announce_start` статичен) и `torrent_enter_seed_only(t)` — подавление
   интереса, `upload_only=1`, перевод selection в «нечего качать».
7. `DownloadStatus::Seeding` + миграция персистентного состояния (скилл
   `debrid` — версионирование download-state).
8. Настройки live-сида в `AppSettingsData` (версионирование — скилл `debrid`).

---

## 3. Целевая архитектура

### 3.1. Жизненный цикл задачи (три фазы)

```
Фаза A: Скачивание + Установка + Live Upload (скользящее окно)
  torrent_tick():
    - качает куски по AIMD-окну (order Coordinator'а);
    - install worker параллельно потребляет чанки в NCM;
    - проверенный кусок копируется в Ring (хук в piece.c до piece_buf_put);
    - бит в available_bf -> HAVE пирам;
    - входящие REQUEST обслуживаются из Ring;
    - при заполнении бюджета старые слоты вытесняются, биты снимаются;
    - запросы вытесненных кусков -> BEP-6 REJECT_REQUEST.

Фаза B: Скачивание завершено + Install Drain + Live Upload
  - все куски скачаны; вызван finishBegin(); download-интерес снят
    (not-interested + upload_only=1, BEP-21: «докачивать не буду»);
  - PackageCoordinator дописывает очередь через finishPoll();
  - torrent_tick() продолжает отдавать из Ring;
  - event=completed на трекер ещё не отправлен (ждём NCM).

Фаза C: Финализация
  Сценарий 1: StreamInstall, игра > кэша:
    - инсталл успешен -> completed + итоговый uploaded -> статус Installed;
    - Ring освобождается под запуск игры; задача завершена.
  Сценарий 2: StreamInstall, игра <= кэша (100% покрытие):
    - инсталл успешен -> completed, left=0 -> статус Seeding;
    - честный seed-only из RAM до тайм-лимита/запуска игры; авто-сон разрешён.
  Сценарий 3: DownloadOnly (полный образ на SD):
    - 100% на SD -> completed, left=0 -> статус Seeding;
    - полноценный сид с SD через storage_read().
```

### 3.2. Матрица флагов и состояний

| Фаза и режим | `want_data` | `upload_enabled` | `seed_only` | `upload_only` (BEP-21) | Битфилд (`available_bf`) | Статус UI | Источник отдачи |
|---|---:|---:|---:|---:|---|---|---|
| StreamInstall: загрузка + инсталл | 1 | 1 | 0 | 0 | окно Ring | Installing (Live seed) | RAM Ring |
| StreamInstall: install drain | 0 | 1 | 0 | 1 | окно Ring | Finalizing install | RAM Ring |
| StreamInstall: финиш, игра > кэша | 0 | 0 | 0 | 0¹ | — | Installed | нет (кэш очищен) |
| StreamInstall: финиш, игра <= кэша | 0 | 1 | 1 | 1 | 100% | Seeding (RAM) | RAM Ring |
| DownloadOnly: загрузка | 1 | 1 | 0 | 0 | готовые куски (disk) | Downloading (Live seed) | SD (`storage_read`) |
| DownloadOnly: финиш | 0 | 1 | 1 | 1 | 100% | Seeding (SD) | SD (`storage_read`) |

¹ Соединения закрываются при завершении задачи — `upload_only` не отправляется.

Примечание: `upload_only=1` при частичном `available_bf` — это ровно кейс
BEP-21 partial seed («докачивать не буду, имею то, что в битфилде»). Ghost
Seeding нет: битфилд всегда отражает реальное наличие.

### 3.3. Правило битфилда отдачи

- Единственный источник правды о servable-кусочках — `available_bf`
  (шлётся в хэндшейке как `our_bf`).
- `have_bf` (наш прогресс) никогда не анонсируется прам и не трогается
  Ring-кэшем — иначе движок посчитает вытесненный кусок «нескачанным» и
  начнёт качать его заново.
- `broadcast_have(idx)` вызывается только когда бит `available_bf[idx]`
  реально установлен (кусок попал в Ring или лежит на диске).
- «Снять бит» из-за вытеснения нельзя выразить протоколом BEP-3 — поэтому
  REJECT/choke-unchoke (см. 4.1), а новые пиры получают уже урезанный битфилд.

---

## 4. Сетевой тракт и протокол отдачи

### 4.1. Входящие `REQUEST`, `CANCEL` и BEP-6 `REJECT_REQUEST`

В `peer.c` (`process_message`, там где константы уже есть — peer.h):

- `MSG_REQUEST` (id 6, payload 12 байт: index, begin, length);
- `MSG_CANCEL` (id 8, payload 12 байт);
- `MSG_REJECT_REQUEST` (id 16, payload 12 байт; константу добавить в peer.h).

Обработка `MSG_REQUEST`:

1. Валидация: `length <= 16 KiB` (BLOCK_SIZE), `begin + length <=
   piece_len(index)`, `index < num_pieces`. Невалид — disconnect (strike).
2. Если пир нами зачокан — игнор.
3. Наличие куска:
   - бит в `available_bf` есть → положить в `up_queue` пира;
   - бита нет (вытеснен из окна):
     - пир с BEP-6 (бит 0x04 в расширенном хэндшейке — уже парсится
       `parse_ext_handshake`) → немедленный `MSG_REJECT_REQUEST`;
     - старый пир → `CHOKE`, очистка очереди, затем `UNCHOKE`.
4. Анти-спам: >16 запросов подряд к отсутствующим кускам → мягкий disconnect;
   пир переподключится и получит актуальный битфилд.

`MSG_CANCEL` удаляет парный элемент из `up_queue`.

### 4.2. Отправка `PIECE` (без упаковки в `sbuf`)

- `sbuf` (peer.h:87, `PEER_BUF_SIZE = 16398`) остаётся для управляющих
  сообщений (`HAVE`, `UNCHOKE`, `CHOKE`, `REJECT_REQUEST`) — они всегда
  помещаются и не отбрасываются.
- `PIECE` отправляется при `POLLOUT` напрямую из Ring-слота (или буфера
  чтения SD для `DownloadOnly`) через существующий неблокирующий `net_send`
  («сколько сокет примет»). Хвост догоняется на следующих тиках: в записи
  `up_queue` хранится `index/begin/length` и текущая позиция отправки.
- Порядок в очереди FIFO; `CANCEL` за летящим хвостом игнорируется (дослать
  начатое — это норма для BEP-3).

### 4.3. Анчок-политика v1 (просто и предсказуемо)

- В `peer_upload_tick()` (новый, вызывается из `torrent_tick` после приёма):
  анчокаем всех `peer_interested` пиров, пока `unchoke_slots` не исчерпан
  (стартово 4 слота); дисинтерес → choke.
- Слоты масштабируются от upload-бюджета (см. 4.4), а не от CPU.
- Ротация/тит-фор-тат — не v1 (см. 1.2), задел оставляем в структуре
  (функция выбора кандидатов отдельная).

### 4.4. Скорость отдачи

- Токен-бакет `live_upload_cap_bps` (по умолчанию 1 МиБ/с, настройка 512 КБ/с
  … без лимита): не создаём bufferbloat входящим пакетам на слабом Wi-Fi
  Switch.
- **Буст при занятом инсталлере:** пока `RequestGate` не `Free`
  (`requestsCurtailed()` координатора — уже есть), upload-лимит расширяется
  до полного потолка канала: Wi-Fi простаивает, когда download приторможен
  записью в NCM.

### 4.5. Учёт отдачи

- `peer_t->uploaded` растёт на каждый отправленный байт `PIECE`
  (в момент успешной `net_send`-порции).
- Агрегат `torrent_t->uploaded` (сумма по пирам, пересчитывается в тик рядом
  рядом с существующим speed-расчётом) кормит трекер-анонсы, телеметрию и
  UI-ratio.

---

## 5. Ring-кэш оперативной памяти

### 5.1. Модуль `seed_cache` (новый, `src/core/seed_cache.{h,c}`)

```c
typedef struct {
    uint32_t  num_slots;
    uint32_t  piece_len;
    uint32_t  head;              /* следующее место записи */
    uint32_t *idx;               /* [num_slots] индекс куска в слоте, UINT32_MAX = пусто */
    uint8_t  *buf;               /* единая arena: num_slots * piece_len */
} seed_cache_t;

seed_cache_t *seed_cache_create(uint32_t num_slots, uint32_t piece_len);
/* Копирует кусок в слот head, возвращает индекс вытесненного куска
   (или -1). Вызывается на torrent-потоке из хука piece.c. */
int      seed_cache_put(seed_cache_t *sc, uint32_t idx, const uint8_t *data, uint32_t plen);
const uint8_t *seed_cache_get(const seed_cache_t *sc, uint32_t idx); /* NULL если нет */
void     seed_cache_destroy(seed_cache_t *sc);
```

- Одна арена вместо N malloc'ов — нулевая фрагментация, дешёвый teardown.
- Количество слотов `N = budget_bytes / piece_length`, выделяется один раз
  при старте задачи (или при первом включении live-сида).

### 5.2. Точка интеграции в `piece.c`

- Проблема: `hash_result_cb` вызывается после `piece_buf_put()` (2.3).
- Решение: новый колбэк `seed_hook(void *user, uint32_t idx, const uint8_t
  *buf, uint32_t plen)` в `piece_mgr_t`, вызываемый в успешной ветке
  `hash_apply`-пути сразу после `mark_piece_done()` и **до**
  `piece_buf_put()`:

```c
mark_piece_done(pm, job->idx, job->plen);
if (pm->seed_hook) {
    int evicted = seed_cache_put(sc, job->idx, job->buf, job->plen);
    if (evicted >= 0)
        bf_clear(pm->available_bf, (uint32_t)evicted); /* +HAVE нового ниже */
    bf_set(pm->available_bf, job->idx);
}
piece_buf_put(pm, job->buf);
```

- `torrent.c` ставит `seed_hook` только когда live-сид включён и задача
  `StreamInstall` (для `DownloadOnly` хук не нужен: кусок и так readable
  на SD — `mark_piece_done` уже ставит бит).
- `broadcast_have(idx)` в этом случае вызывает сам torrent-слой (гейт по
  3.3), существующие вызовы в `on_block`-пути для `StreamInstall`
  подавляются.

### 5.3. Бюджет и `StreamBudgetArbiter`

- Потребление памяти контролируется арбитром:
  - **Applet Mode** (~400 МБ свободно): Ring ограничен 64–128 МБ или
    отключён (тогда live-сид только для `DownloadOnly`-кусков на SD).
  - **Title Override** (~3.2 ГБ):
    `RAM_total = EngineOverhead + CoordinatorBuffer + RingCache + Reserve`,
    где EngineOverhead ≈ 64 МБ/слот, CoordinatorBuffer ≤ 384 МБ (пик
    арбитра), Reserve ≥ 256 МБ.
- Пользовательский выбор (256/512/1024 МБ или «Авто») валидируется против
  `detectStreamRamMemorySnapshot()`; вариант «2 ГБ» блокируется при < 2.8 ГБ
  доступной кучи.
- Резайз арбитром в полёте (`applyBudget`) для Ring v1 не делаем: кэш
  фиксируется на старте задачи; изменение настройки применяется к следующей
  задаче. (Задел: пересчёт слотов с вытеснением хвоста.)

### 5.4. Потокобезопасность

- Запись: `seed_cache_put` из `piece_mgr_drain_hash_results()` внутри
  `torrent_tick()`.
- Чтение: `seed_cache_get` при формировании `PIECE` в upload-пути того же
  `torrent_tick()`.
- Оба конца на одном потоке → мьютексы не нужны. `DownloadManager::mutex_`
  используется только для копирования телеметрии в UI, как сейчас.

---

## 6. Неблокирующий Install Drain

### 6.1. `PackageCoordinator` (src/app/package_coordinator.hpp)

```cpp
enum class DrainState { Running, Done, Error };

void finishBegin();       // неблокирующе: accepting_=false; queueReady_.notify_all()
DrainState finishPoll();  // неблокирующе: под queueMutex_ проверить тот же
                          // предикат, что у drained_.wait в finish():
                          // error_ || (pending_ && queue_ пусты && !processing_)
bool finish();            // блокирующая обёртка для тестов: begin + wait
```

- Существующий `finish()` переписывается как `finishBegin()` + ожидание
  (поведение совместимо; все текущие вызовы не меняются).
- `finishPoll()` возвращает `Done` только когда install worker дописал и
  закоммитил все пакеты (или ошибку через `error()`).

### 6.2. Главный цикл `runTask()` (download_manager.cpp ~:3320)

Сейчас: `bool installOk = coordinator->finish();` — блокирует поток, отдача
умирает на drain. Замена:

```cpp
// при running == 0 и mode == StreamInstall и !drainStarted:
//   coordinator->finishBegin(); drainStarted = true;
// в цикле, пока drainStarted:
//   DrainState st = coordinator->finishPoll();
//   torrent_tick(torrent);            // отдача продолжается
//   updateTaskProgress();             // статус Finalizing install
//   if (st == DrainState::Running) continue;
//   st == Error  -> task->status = Error; break;
//   st == Done   -> сценарий C из 3.1:
//     - packageFullyCached (игра <= Ring) и seedAfterInstall:
//         torrent_request_tracker_event(torrent, TRACKER_EVENT_COMPLETED);
//         torrent_enter_seed_only(torrent);
//         task->status = DownloadStatus::Seeding;
//     - иначе:
//         torrent_request_tracker_event(torrent, TRACKER_EVENT_COMPLETED);
//         task->status = DownloadStatus::Installed; break; // Ring освободится
```

- Для `StreamInstall` авто-`completed` из torrent.c:2098 подавляется
  флагом на `torrent_t` (ставится при создании, см. фазу 3): движок не шлёт
  `completed` сам; менеджер шлёт его ровно после `Done`.
- Для `DownloadOnly` поведение torrent.c:2098 остаётся (100% на SD — это
  честный completed) + переход в `Seeding`.

---

## 7. Трекер, честность и приватность

### 7.1. Правила анонсов

1. `uploaded` (HTTP и UDP): реальные байты сессии отдачи (`t->uploaded`).
2. `downloaded` (HTTP и UDP): реальные принятые байты сессии (`t->downloaded`,
   не `completed_bytes`) — фикс `torrent_announce_totals`.
3. `left`:
   - при скачивании: `total - completed_bytes`;
   - в `Seeding`: строго `0`.
4. `event=completed`:
   - `DownloadOnly`: при 100% на SD (существующее поведение);
   - `StreamInstall`: только после `finishPoll() == Done` (подавление
     авто-отправки, см. 6.2).
5. `event=stopped`: при выходе/паузе/завершении с актуальными `uploaded` и
   `downloaded` (расширить структуру `stopped_announce`-джобы).

### 7.2. Приватные торренты (BEP-27)

- Парсинг `info.private=1` в `metainfo.c` → `metainfo_t::is_private`.
- Если приватный: `dht_attach()` не вызывается; PEX-флаг из ext-хэндшейка
  сбрасывается (не предлагаем), входящие PEX-сообщения игнорируются
  (`parse_pex` гейтится); passkey/ключи трекеров маскируются в логах
  (`passkey=***`).

---

## 8. Питание, сон и системные ресурсы

### 8.1. Auto-Sleep

- `hasActiveTransfer()` (download_manager.cpp:1847) возвращает `true` для
  `Downloading`, `Installing`, `Committing` (блокируем сон — нельзя
  прерывать FS/NCM) и `false` для `DownloadStatus::Seeding`.
- Консоль в портативном режиме с активным сидом штатно заснёт по таймеру
  пользователя — разряд в 0% исключён.

### 8.2. Выход из приложения / запуск игры

- `event=stopped` с финальной статистикой (7.1.5), буферы Ring освобождаются
  (`seed_cache_destroy` в `torrent_destroy`).

---

## 9. Пользовательский интерфейс

### 9.1. Настройки (`Настройки → Загрузки / BitTorrent`)

- Тумблер: «Раздавать во время загрузки (Live Seeding)» — default ON.
- Размер кэша: `256 МБ / 512 МБ (default) / 1 ГБ / Авто`.
- Лимит отдачи: `512 КБ/с / 1.0 МБ/с (default) / 2.0 МБ/с / Без ограничений`.
- Тумблер: «Продолжать раздачу после установки (для игр ≤ размера кэша)» —
  default OFF.
- Ключи добавляются в `en-US` → `ru`, `pt-BR`, `fr`; `check_i18n.py`.

### 9.2. Карточки задач

- `Downloading`/`Installing`: бейдж `↑ LIVE [скорость]` + отданный объём.
- `Finalizing install`: «Запись в память консоли», бейдж `↑ LIVE` активен.
- `Installed`: «Установлено · Отдано за установку: X.X ГБ · Ratio X.XX».
- `Seeding`: «Раздача (RAM / SD) · ↑ X.X МБ/с · Ratio X.XX».
- В `DownloadTask` добавляются поля `uploadedBytes`, `uploadSpeedBytesPerSecond`
  (копируются из `torrent_stat` под `mutex_`, как остальная телеметрия).

---

## 10. Поэтапный план реализации

Каждая фаза — отдельный мердж-рикуест: `main` → ветка → `--ff-only`.
Фазы 0–1 не зависят друг от друга; 2 зависит от 1; 3 от 0; 4 от 1–3; 5 от 4.

### Фаза 0 — Честный трекер и приватность (S)

**Файлы:** `src/core/metainfo.{c,h}`, `src/core/tracker.{c,h}`,
`src/core/torrent.c` (гейт `dht_attach` на torrent.c:1865 и `flush_pex`
на torrent.c:951), `src/core/peer.c` (PEX-флаг в ext-хэндшейке).
`src/core/dht.c` не меняется.

- [ ] `metainfo.c`: парсинг `info.private` → `is_private`.
- [ ] Приватный торрент: без `dht_attach()`, PEX не предлагаем и не парсим,
      маскирование passkey в логах.
- [ ] `tracker.c`: `uploaded`-параметр в `http_build_announce_url` (замена
      `&uploaded=0`) и запись int64 на offset 72 в
      `udp_build_announce_packet` (BEP-15).
- [ ] `torrent.c`: `torrent_announce_totals` — `downloaded = t->downloaded`;
      `stopped`-джоба несёт `uploaded`; announce-каскад прокидывает `uploaded`.
  (Сами значения `uploaded` ещё нулевые — считать начнём в фазе 1; здесь
  прокидывается plumbing.)
- [ ] Тесты: `tests/test_tracker.c` — фикстура announce-URL и UDP-пакета
      (байты 72–79 = ожидаемый uploaded), приватный metainfo-парсинг;
      регистрация в `Makefile.pc`.

**DoD:** `make -f Makefile.pc test`, `make pc`, `make switch`.

### Фаза 1 — Upload data path в peer.c (M)

**Файлы:** `src/core/peer.{c,h}`, `src/core/torrent.c` (upload-тик и агрегат),
`src/core/net.h` (при необходимости).

- [ ] Счётчики: `peer_t->uploaded`, `torrent_t->uploaded`; агрегация в тик.
- [ ] `MSG_REQUEST`/`MSG_CANCEL` в `process_message` (валидация 4.1,
      `up_queue` на пира, анти-спам).
- [ ] `MSG_REJECT_REQUEST` (id 16 в peer.h): константа, отправка, приём
      (сброс нашего ожидания — defensive).
- [ ] `peer_send_unchoke()`/`peer_send_choke()` управление; слоты 4.3.
- [ ] Отправка `PIECE` при `POLLOUT` через `net_send` из источника
      (пока единственный источник — тестовый буфер; SD/Ring подключаются
      фазами 2 и 4).
- [ ] Честный interest: не слать `interested`, если нечего качать;
      `not-interested` при завершении выбора кусков.
- [ ] `upload_only` (BEP-21) из состояния (пока всегда 0 — включается фазой 4).
- [ ] Токен-бакет `live_upload_cap_bps` в `torrent_tick`.
- [ ] Тесты: расширить `tests/test_peer.c` — валидация REQUEST, REJECT для
      отсутствующего куска, CANCEL, лимит 16 КиБ, анчок-слоты, счётчик
      uploaded.

**DoD:** сборочная матрица; в `test_peer.c` нет сетевых зависимостей.

### Фаза 2 — Ring-кэш seed_cache (M, ядро)

**Файлы:** новые `src/core/seed_cache.{h,c}` (регистр в `Makefile.pc`
`CORE_SOURCES` и в CMake Switch-список), `src/core/piece.{c,h}` (seed_hook),
`src/core/torrent.c` (установка хука, гейт broadcast_have).

- [ ] `seed_cache` по 5.1 (арена, ring, put/get, вытеснение).
- [ ] `seed_hook` в `piece_mgr_t`; вызов до `piece_buf_put` (5.2).
- [ ] Интеграция битфилда: `available_bf` ставится/снимается, `HAVE` только
      по факту (3.3); для `DownloadOnly` ничего не меняется.
- [ ] Тесты: `tests/test_seed_cache.c` — put/get/вытеснение, битфилд после
      вытеснения, `broadcast_have` не зовётся для вытесненного; расширить
      `tests/test_piece.c` — порядок seed_hook → piece_buf_put.

**DoD:** сборочная матрица (новые файлы — в оба билда!).

### Фаза 3 — Неблокирующий Install Drain (M)

**Файлы:** `src/app/package_coordinator.hpp`, `src/app/download_manager.cpp`,
`src/core/torrent.{c,h}`.

- [ ] `DrainState`, `finishBegin()`, `finishPoll()`; `finish()` как обёртка.
- [ ] `runTask()`: стейт-машина из 6.2 (drainStarted, тик продолжается,
      статусы `Finalizing install`).
- [ ] Подавление авто-`completed` для `StreamInstall` (флаг на torrent_t) +
      публичный `torrent_request_tracker_event()`.
- [ ] `check_completion` для SINK не трогает диск (уже так и есть по 2.4 —
      убедиться, что новых дисковых вызовов в drain-пути нет).
- [ ] Тесты: расширить `tests/test_package_coordinator.cpp` — finishBegin/
      finishPoll не блокируют, Done только после полной очереди, Error
      пробрасывается; `tests/test_task_progress.cpp` — статусы drain.

**DoD:** сборочная матрица.

### Фаза 4 — Режим Seeding, бюджет кэша, энергосбережение (M)

**Файлы:** `src/app/download_manager.{cpp,hpp}`, `src/app/app_settings.{cpp,hpp}`
(скилл `debrid` — версионирование), `src/core/torrent.c`, `src/app/stream_budget_arbiter.hpp`.

- [ ] `DownloadStatus::Seeding` + миграция персистентного состояния
      (версия download-state, скилл `debrid`).
- [ ] `torrent_enter_seed_only()`: not-interested, `upload_only=1`,
      источник отдачи — Ring или SD (`storage_read` для DISK-кусков).
- [ ] Сценарии C: RAM-сид только при `игра <= кэша`; иначе `Installed` +
      teardown. `DownloadOnly` → сид с SD.
- [ ] Бюджет Ring через арбитр (5.3): валидация против снапшота памяти,
      Applet-урезание, фиксация на старте задачи.
- [ ] `hasActiveTransfer()`: `Seeding` не блокирует сон.
- [ ] 1 активный seed-слот: вторая задача при старте сид-фазы ждёт/финишится
      (простая очередь).
- [ ] Тесты: `tests/test_manager.cpp` (последний!) — переход в Seeding,
      сид с SD, Installed для большого StreamInstall; расширить
      `tests/test_stream_ram_budget.cpp` — бюджет Ring.

**DoD:** сборочная матрица; ручная проверка авто-сна на консоли (checklist в
PR-описании).

### Фаза 5 — UI, настройки, телеметрия (M)

**Файлы:** `src/app/app_settings.{cpp,hpp}` (скилл `debrid`),
UI-панель настроек (`src/main_switch.cpp` / соответствующий tab), карточки
задач, `resources/locales/*`.

- [ ] Настройки 9.1 в `AppSettingsData` + версионирование настроек.
- [ ] Бейджи и статусы 9.2, `uploadedBytes`/`uploadSpeed` в `DownloadTask`.
- [ ] Телеметрия: `telemetry_log("seed", ...)` — uploaded, доля REJECT,
      evictions, cache_hit_ratio, unchoke_slots.
- [ ] Локализация `en-US` → `ru`, `pt-BR`, `fr`; `check_i18n.py` зелёный.

**DoD:** сборочная матрица + `python3 scripts/check_i18n.py`.

---

## 11. Критерии приёмки

1. **Live-отдача:** сторонний BitTorrent-клиент скачивает блоки со Switch во
   время установки; `uploaded` растёт до конца инсталла.
2. **Drain непрерывен:** в фазе финализации NCM скорость отдачи не падает
   в 0 (лог `buffer`/`seed`-телеметрии).
3. **Память стабильна:** 2 часа live-сида — потребление кучи ограничено
   бюджетом, `PIECE_BUF_POOL_MAX` не деградирует (нет ростовых malloc в
   логе), утечек нет.
4. **Честность:** `completed` только после успешного инсталла; HTTP и UDP
   трекеры видят корректные `uploaded`/`downloaded` (offset 72 в UDP-дампе).
5. **Приватность:** на `private=1` торренте DHT-роутеры не пингуются, PEX
   не рассылается, passkey в логах замаскирован.
6. **Батарея:** консоль в `Seeding` засыпает по системному таймеру.
7. **Сборки и тесты:** `make -f Makefile.pc test`, `make pc`, `make switch`,
   `python3 scripts/check_i18n.py` — зелёные.

---

## 12. Риски и открытые вопросы

| Риск | Митигация |
|---|---|
| Входящие пиры за NAT до Switch не достучатся (μTP inbound отвергается) | Раздача идёт исходящим соединениям (мы и так подключены к рою) + входящий TCP; приём μTP-inbound — кандидат в v2 |
| REJECT-шторм от отставших пиров | Анти-спам 4.1.4; disconnect → переподключение с актуальным битфилдом |
| Ring-арена + CoordinatorBuffer вместе превышают бюджет в Applet | Ring в Applet ограничен 64–128 МБ или off (5.3); валидация по `detectStreamRamMemorySnapshot()` |
| Захочется Resize Ring в полёте | v1 не делаем (5.3); арена едина — задел под пересоздание при вытеснении хвоста |
| `Seeding` в персистентном состоянии ломает загрузку старых сессий | Версионирование download-state по скиллу `debrid`; неизвестный статус маппится в `Completed` |
| Тест live-сида требует внешнего пира | `test_peer.c`/`test_seed_cache.c` — чистые юниты на loopback-транспорте, живой рой только в `test_manager` (последний) |

Открытые вопросы (решить до/во время фаз, не блокируют старт):

1. Дефолт live-сида ON при первом включении — подтвердить продакт-решением
   (PR фазы 5).
2. Анчок-слоты: фикс 4 или от upload-лимита (4.3) — снять замер на
   железе в фазе 1.
3. Тайм-лимит RAM-сида после инсталла (сценарий C2): фиксированный
   (например, до запуска игры) или настраиваемый — v1: до запуска игры
   или выхода, без таймера.
