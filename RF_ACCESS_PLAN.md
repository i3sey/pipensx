# План: доступ к торрентам в РФ (RuTracker заблокирован)

Задачи разбиты по сложности и назначены на тир модели-исполнителя:

- **Fable** — сложные: новые подсистемы, конкурентность, высокий blast-radius в core, кросс-системные изменения (CI + клиент + схема + модель доверия). Требуют архитектурных решений и аккуратной обработки крайних случаев.
- **Opus** — средние: нетривиальная логика внутри одного модуля (криптопроверка, мульти-источниковый фолбэк, протокольная обвязка), но по готовому паттерну.
- **Sonnet** — простые: плейбук уже есть в кодовой базе, копирование паттерна, плейсменты строк, пламбинг настроек, скриптовая обвязка.

Порядок разделов — от сложной к простой.

## Сводная таблица

| ID | Задача | Тир | Оценка | Зависит от |
|----|--------|-----|--------|-----------|
| П2.1 | Серверный пре-резолв: info-dict в каталог ✅ | Fable | L | П3.2 (желательно) |
| П1.1 | DHT-фолбэк в резолв магнита ✅ | Fable | L | — |
| П0.1 | Анонс трекера через antizapret-прокси ✅ | Fable | M | — |
| П3.2 | Подпись каталога (ed25519) ✅ | Opus | M | — |
| П3.1 | Зеркала каталога (jsDelivr / R2 / IPFS) ✅ | Opus | M | — |
| П1.2 | PEX-усиление тонкого списка пиров ✅ | Opus | M | П0.1 или П1.1 |
| П0.3 | Тумблер / автодетект antizapret ✅ | Sonnet | S | П0.1, П0.2 |
| П0.2 | Фетч каталога через antizapret-прокси ✅ | Sonnet | S | — |
| ПX.1 | Health-check через прокси в CI | Sonnet | S | — |
| ПX.2 | Публичные open-трекеры в кандидаты | Sonnet | S | — |
| ПX.3 | Телеметрия источника резолва | Sonnet | S | — |

Минимальный набор для работы в РФ: **П0.1 + П0.2 + П1.1** — ✅ готов целиком (2026-07-03).

Следующая крупная веха — **Фаза W** (мастер первоначальной настройки и лестница подключения), см. отдельный раздел ниже.

---

## Фаза W — Мастер первоначальной настройки и лестница подключения

**Статус: движок готов (W0 ✅ W1 ✅ W2 ✅), UI-флоу впереди (W3–W6).** Собрать все механизмы обхода
(П0.1/П0.2/антизапрет/DHT/зеркала/пре-резолв) в один управляемый пользователем
поток. Сейчас обход зашит и срабатывает молча; цель — явная лестница эскалации
с одноразовым мастером на первом запуске, где пользователь может подставить свой
прокси или переключиться на зеркало RuTracker, а в самом худшем случае получает
старый дамп с честным предупреждением.

### Лестница подключения (порядок попыток)

```
[1] Прямой стук к RuTracker (announce/каталог напрямую)
      │ ok → работаем, метод запомнен
      │ fail (DPI-заглушка / таймаут / reset)
      ▼
[2] Одноразовый попап (первый запуск): «RuTracker недоступен напрямую»
      ├─ у пользователя выставлен свой прокси в настройках? → используем его
      └─ нет → пробуем антизапрет-маршруты + прочие методы обхода (DHT и т.д.)
      │ ok → работаем, победивший метод сохранён (connectivitySetupDone=1)
      │ fail
      ▼
[3] Сообщаем «обойти не удалось», предлагаем сменить rutracker.org
    на зеркало RuTracker (rutrackerHost override)
      │ пользователь согласился и зеркало ожило → работаем
      │ отказ / зеркало тоже мертво
      ▼
[4] Грузим старый bundled-дамп + баннер-предупреждение (данные устарели,
    возраст снапшота показан)
```

Ключевой принцип: лестница проходится **один раз** (мастер), победивший
маршрут пишется в настройки, дальнейшие запуски идут по нему молча. Мастер
перезапускаем из настроек (W6). Каждый шаг — явный, отменяемый, с понятным
текстом; никакого молчаливого фолбэка на устаревшие данные.

### Подзадачи

| ID | Задача | Тир | Оценка | Зависит от |
|----|--------|-----|--------|-----------|
| W2 | Оркестратор лестницы (state machine) ✅ | Fable | L | W0, W1 |
| W3 | UI мастера (borealis first-run flow) | Fable | L | W0, W2 |
| W1 | Проба доступности RuTracker по маршруту ✅ | Opus | M | — |
| W4 | Override хоста трекера/каталога (зеркало) | Opus | M | W0 |
| W0 | Схема настроек: proxy / antizapret / mirror / first-run ✅ | Sonnet | S | П0.3 |
| W5 | Баннер «устаревший дамп» + возраст снапшота | Sonnet | S | W0 |
| W6 | Перезапуск мастера из настроек | Sonnet | S | W2, W3 |

### W0 — Схема настроек ✅
**Оценка: S. Тир: Sonnet.**

**Сделано (2026-07-06):** поля Фазы W добавлены в `AppSettingsData`
(`src/app/app_settings.hpp`), все дефолты сохраняют текущее поведение:
- `manualProxyUrl` (пусто) + `manualProxyType` (`ProxyType` off/http/socks5,
  дефолт Off);
- `useAntizapret` — уже был из [[П0.3]];
- `rutrackerHost` (пусто = `rutracker.org`);
- `connectivitySetupDone` (false);
- `connectivityMethod` (`ConnectivityMethod` direct/proxy/antizapret/mirror,
  дефолт Direct).

Пробросано через `parseSettings`/`serializeSettings`/`operator==` по паттерну
существующих enum-полей (name-хелперы `proxyTypeName`/`connectivityMethodName`,
валидация неизвестных значений → fail-closed). JSON-ключи: `manual_proxy_url`,
`manual_proxy_type`, `rutracker_host`, `connectivity_setup_done`,
`connectivity_method`. Тест `test_app_settings` расширен (дефолты + round-trip),
зелёный.

Исходное описание полей:
- `std::string manualProxyUrl` + `ProxyType manualProxyType` (off/http/socks5) —
  прокси пользователя;
- `bool useAntizapret` (тумблер из П0.3);
- `std::string rutrackerHost` (override, пусто = `rutracker.org` по умолчанию);
- `bool connectivitySetupDone` (флаг пройденного мастера);
- `int connectivityMethod` (запомненный победивший маршрут: direct/proxy/az/mirror).

**Почему Sonnet:** пламбинг полей по готовому паттерну сериализации.

### W1 — Проба доступности ✅
**Оценка: M. Тир: Opus.**

**Сделано (2026-07-06):** `tracker_probe(announce_url, route, timeout_seconds,
cancel_cb, cancel_user) → {REACHABLE, BLOCKED, TIMEOUT}` в `src/core/tracker.c`
(объявление в `tracker.h`). Переиспользует всю curl-обвязку announce
(`curl_buf_t`, `curl_write_cb`, cancel через `CURLOPT_XFERINFOFUNCTION`,
`antizapret_apply_route`, TLS1.2-пиннинг):
- Шлёт лёгкий announce-«заглушку»: нулевой info_hash + `numwant=0`. Живой
  RuTracker-трекер отвечает коротким bencode `failure reason: torrent not
  registered`, при этом запрос фреймится ровно как настоящий announce (тот же
  путь, что инспектит DPI). Короткий таймаут (дефолт 8 c, параметризуем),
  отменяемость.
- Классификация вынесена в чистую `tracker_probe_classify(transport_ok,
  status, body, body_len)` (в `tracker.h`, тестируется офлайн):
  транспортная ошибка/отмена → **TIMEOUT**; тело матчит
  `antizapret_response_looks_blocked` → **BLOCKED** (приоритет над статусом);
  2xx + валидный bencode-dict → **REACHABLE**; иначе (завершилось, но это не
  трекер: proxy/captive-страница, не-2xx, мусор) → **BLOCKED**.
- Лог `[probe] <url> via <route> -> <verdict>` (телеметрия, ПX.3).
- Тест `tests/test_tracker_probe.c` (`tracker_probe_classify`: timeout / DPI-стаб /
  not-registered / peers-dict / non-tracker HTML / non-2xx / пустое 2xx-тело),
  прописан в `Makefile.pc` (`test`-таргет). Live-проверено: `example.com`
  (2xx HTML, не трекер) → BLOCKED, `bt.t-ru.org` напрямую → REACHABLE.

Оркестратор (W2) и обычный резолв зовут `tracker_probe` на каждом маршруте;
здесь только функция + классификатор, без перепроводки резолва.

Исходное описание: функция `probeRuTracker(route, host) → {Reachable, Blocked,
Timeout}`: лёгкий HTTP-запрос (announce-заглушка или HEAD каталога) через
заданный маршрут, детект DPI-блока через `antizapret_response_looks_blocked`
(`src/core/antizapret.c`). Короткий таймаут, отменяемость.

**Почему Opus:** сетевая проба + классификация ответа (успех / блок / таймаут),
аккуратные таймауты, но по паттерну существующих announce-вызовов.

### W2 — Оркестратор лестницы ✅
**Оценка: L. Тир: Fable.**

**Сделано (2026-07-06):** state machine лестницы в новом модуле
`src/app/connectivity_orchestrator.{hpp,cpp}`. Ядро разбито на чистые функции
(тестируемые офлайн — весь сетевой I/O инъектируется как `ProbeFn`) плюс
асинхронный драйвер:
- `buildLadder(config)` — упорядоченные ступени
  direct → manualProxy → antizapret-маршруты (в порядке PAC) → mirror.
- `runLadder(config, probe, onEvent, cancel, user)` — перебирает ступени, на
  каждой зовёт W1-пробу (инъекция `tracker_probe`), при первом `REACHABLE`
  фиксирует `LadderResult{Connected, method, label}` и останавливается; эмитит
  события `RunStarted`/`AttemptStarted`/`AttemptFinished`/`RunFinished` для UI
  (W3). Отмена проверяется до и после каждой пробы (отмена во время пробы
  приходит как TIMEOUT, но явный запрос → `Cancelled`). Телеметрия
  `[wizard] rung <label> -> <verdict>` (ПX.3).
- Терминалы: все ступени провалились и зеркала не было → `NeedMirror` (мастер
  предложит зеркало); зеркало было и тоже упало → `Exhausted` (устаревший
  дамп). Победа → метод из ступени (Direct/Proxy/Antizapret/Mirror).
- `applyLadderResult(result, settings&)` — только `Connected` пишет
  `connectivityMethod` + `connectivitySetupDone`; любой другой исход оставляет
  настройки нетронутыми, чтобы мастер прошёл заново, а не молча отдал устаревшие
  данные.
- `manualProxyRoute` (W0-настройки → `antizapret_route_t`) и
  `deriveMirrorAnnounce` (подмена хоста в announce; принимает только bare
  host[:port] — жёсткую валидацию против списка зеркал добавит W4) — чистые,
  тестируемые. `ladderConfigFromSettings` — тонкий адаптер над глобалами
  (`antizapret_get_routes`, фильтр `antizapret_route_supported`: HTTPS-прокси на
  Switch отсекается), собирает `LadderConfig` из `AppSettingsData`.
- `ConnectivityOrchestrator` — фоновый поток гоняет `runLadder` (проба через
  `tracker_probe` с прокинутым в curl cancel для быстрой отмены), события
  кладутся в потокобезопасную очередь, UI-поток дренирует их `poll(sink)`
  каждый кадр; API `start`/`startWithProbe` (тест-шов)/`cancel`/`running`,
  деструктор отменяет+джойнит. Event-loop не блокируется.
- Тест `tests/test_connectivity_orchestrator.cpp` (13 кейсов: порядок ступеней,
  ранняя остановка на direct, фолбэк на antizapret, приоритет manualProxy,
  NeedMirror / Exhausted / mirror-success, отмена в чистом `runLadder` и в
  асинхронном драйвере, `applyLadderResult`, конверсия прокси, подмена хоста),
  прописан в `Makefile.pc`; `connectivity_orchestrator.cpp` добавлен в
  `APP_SERVICE_SOURCES` (CMake, Switch-сборка). Полный PC-набор зелёный.

Интеграция в first-run UI и гейт на `connectivitySetupDone` — за W3; здесь
только движок, события и пробрасывание настроек.

Исходное описание: state machine, реализующая лестницу выше: перебирает
маршруты (direct → manualProxy → antizapret-routes → mirror-swap → stale-dump),
на каждом шаге зовёт W1, при успехе фиксирует `connectivityMethod` +
`connectivitySetupDone` и останавливается. Управляет переходами мастера, эмитит
события для UI (W3). Отменяемость, отсутствие блокировки event-loop.

**Почему Fable:** новая подсистема, кросс-модульная (настройки + резолвер +
каталог + антизапрет + UI), развилки состояний и крайние случаи (частичный
успех, отмена в середине, повторный запуск).

### W3 — UI мастера
**Оценка: L. Тир: Fable.**

Borealis-флоу первого запуска: экраны «проверяем подключение», «нет прямого
доступа — задать прокси или пробовать обход?», ввод прокси, «обойти не удалось —
переключиться на зеркало <host>?», «работаем в офлайне на старом дампе»
(предупреждение). Гейт на `connectivitySetupDone`. Прогресс/отмена завязаны на
события W2.

**Почему Fable:** новый многошаговый UI-флоу с состояниями, завязан на
асинхронный оркестратор, high-touch по UX.

### W4 — Override хоста (зеркало)
**Оценка: M. Тир: Opus.**

Сейчас `allowedTracker` (`src/app/magnet_resolver.cpp:79`) хардкодит
`bt*.t-ru.org`, а каталог-хосты — в аллоулисте (`isTrustedSource`). Пробросить
`rutrackerHost` из настроек: разрешить сконфигурированное зеркало в белом списке
трекеров и подменять хост в magnet `tr=` / каталожных запросах. Валидировать
зеркало против списка известных зеркал RuTracker (не произвольный хост).

**Почему Opus:** обобщение белого списка + подмена хоста в резолве, локально, но
с требованием не открыть SSRF/подмену на произвольный домен.

### W5 — Баннер «устаревший дамп»
**Оценка: S. Тир: Sonnet.**

Когда каталог пришёл из bundled-снапшота (`sourceLabel_` = bundled), показать
баннер с возрастом снапшота (`catalogGeneratedAt`) и явным «данные могут быть
устаревшими». Каркас у `CatalogService::sourceLabel()` уже есть.

**Почему Sonnet:** одна UI-строка по существующему состоянию источника.

### W6 — Перезапуск мастера из настроек
**Оценка: S. Тир: Sonnet.**

Пункт в настройках «Настроить подключение заново»: сбрасывает
`connectivitySetupDone`, запускает флоу W3. Пламбинг действия.

**Почему Sonnet:** кнопка + сброс флага по готовым паттернам настроек.

---

## Тир Fable (сложные)

### П2.1 — Серверный пре-резолв: info-dict в каталог ✅
**Оценка: L. Blast-radius: высокий (CI + схема каталога + клиентский резолв + модель доверия).**

Резолвить магниты до полного info-словаря в CI (вне РФ или через прокси) и класть его (base64) прямо в записи каталога. Клиент в РФ тогда пропускает фазу «трекер → пир → ut_metadata» целиком.

**Сделано (2026-07-03):**
- `CatalogEntry.infoDict` + декод/верификация в `parseJson`
  (`src/app/catalog_service.cpp`): base64 → SHA-1 сверка с btih магнита прямо
  при парсинге. Не сошёлся hash или битый base64 → поле чистится, запись
  остаётся годной для сетевого резолва (доверие: непустой `infoDict` = всегда
  проверенный).
- `resolveToFile` получил параметр `presetInfo`: валиден против магнит-хэша →
  `buildTorrent` + атомарная запись, вся сетевая фаза пропущена; иначе фолбэк
  на обычный резолв (`buildTorrent` уже валидировал hash — подмена не пройдёт).
- Резолвер-колбэк прокинут по `CatalogEntry` (одиночный install в
  `main_switch.cpp` + `CatalogBatchInstaller`), чтобы `infoDict` дошёл до
  резолва.
- CI-скрипт `tools/embed_catalog_infodicts.py`: announce → BEP10/BEP9
  ut_metadata (конкурентный пул пиров) → base64 в `info_dict`, бюджет байт под
  16 МБ лимит клиента, `--proxy`, `--only-missing`.
- Тесты: `testInfoDictParsing` (валид/mismatch/битый base64),
  `testResolveFromPresetInfoDict` (offline-резолв + фолбэк). Live-проверено
  end-to-end: тул embed'нул info-dict за 7 c, клиент со ВСЕЙ отрезанной сетью
  (connect+sendto→ENETUNREACH) собрал валидный .torrent из каталога.

**Почему Fable:** меняет правила игры (клиент вообще не ходит на RuTracker), спанит CI + клиент + формат каталога + доверие. Развилка в резолве и валидация встроенного словаря требуют аккуратности.

### П1.1 — DHT-фолбэк в резолв магнита ✅
**Оценка: L. Blast-radius: средний (новый event-loop, потоки, интеграция vendor).**

**Сделано (2026-07-03):** DHT-поиск стартует в отдельном потоке параллельно
tracker-циклу в `resolveToFile` (движок — существующий `src/core/dht.c`).
Трекеры ответили (пиры или «not registered») → поиск гасится сразу; трекеры
недоступны → работает до 25 с / 32 пиров, результат мержится через
`appendUniquePeers`. jech dht — глобальный синглтон, поэтому в
`dht_engine_create/destroy` добавлен атомарный guard: при активной закачке
резолвер молча идёт без DHT. Live-проверено с отрезанными трекерами и
antizapret-прокси: DHT дал 102 пира, метаданные получены, .torrent собран.

Обнаружение пиров без RuTracker-трекеров. jech `vendor/dht/dht.c` уже в дереве (bootstrap+search проверены на PC), но в `resolveToFile` не подключён.

- Перед/параллельно tracker-анонсу запустить короткий DHT `get_peers` по `spec.infoHash` (15–30 c, UDP), мержить пиров через `appendUniquePeers` (`src/app/magnet_resolver.cpp:123`).
- Bootstrap-ноды (`router.bittorrent.com`, `dht.transmissionbt.com`, `router.utorrent.com`) — UDP, РКН не блокирует.
- Мини event-loop с `dht_periodic` на время резолва, в отдельном потоке (потоковая модель воркеров уже есть в `resolveToFile:600`).

**Почему Fable:** новая подсистема, конкурентность, UDP event-loop, интеграция vendor-кода. Убирает единую точку отказа.

### П0.1 — Анонс трекера через antizapret-прокси ✅
**Оценка: M. Blast-radius: высокий (core `tracker.c`, нет покрывающих тестов).**

**Уже реализован** (коммит `aca354a`, план писался по устаревшему снимку кода):
`http_announce` давно на libcurl, все пункты ниже на месте —
`antizapret_apply_route` (`tracker.c:132`), `antizapret_response_looks_blocked`
(`:151`), скип маршрутов без `antizapret_route_supported` (`:225`),
cancel-callback через CURLOPT_XFERINFOFUNCTION (`:123`) с заполнением
`tracker_announce_result_t`, antizapret в CORE_SOURCES. Сверх плана: залипание
на прокси после первого успеха (`antizapret_proxy_preferred`) и 3 ретрая на
маршрут. Live-проверено 2026-07-03: с отрезанным портом 80 анонс ушёл через
antizapret-прокси (`[tracker] RuTracker announce used HTTP proxy`).

Исходное описание (устарело): `http_announce` (`src/core/tracker.c`) работает на сыром сокете, без прокси. Домены `t-ru.org` уже в `host_is_target` (`src/core/antizapret.c:50`) — маршрутизация включится сама.

- Переписать `http_announce` на libcurl (это обычный HTTP GET, тело — bencode) и звать `antizapret_apply_route()` перед `curl_easy_perform`, как в `src/app/game_metadata_service.cpp:242`.
- Реюз `antizapret_response_looks_blocked` для детекта DPI-заглушек.
- На Switch учесть: `antizapret_route_supported` (`src/core/antizapret.c:340`) выключает HTTPS-прокси → использовать SOCKS5/HTTP-маршруты.
- Сохранить cancel-callback (`tracker_cancel_cb`) и фрейминг результата.
- Прилинковать antizapret к core-таргету в `CMakeLists.txt`.

**Почему Fable:** переписывает core-функцию без тестового покрытия, высокий blast-radius (2 вызова из `torrent.c` + резолвер), Switch-специфика curl. Максимальный ROI — вся инфраструктура обхода уже стоит, не доведена только до этого вызова.

---

## Тир Opus (средние)

### П3.2 — Подпись каталога (ed25519) ✅
**Оценка: M.**

**Сделано (2026-07-06):** detached ed25519-подпись сетевого каталога,
verify-only на клиенте.
- Крипто: вендор TweetNaCl (`vendor/tweetnacl/`, канон 20140427, public
  domain) — verify-only, свой SHA-512, без зависимости от mbedTLS/OpenSSL
  (mbedcrypto на Switch ed25519 не умеет). Обёртка
  `src/core/catalog_sig.c` (`catalog_sig_verify`): собирает sig(64)‖msg,
  зовёт `crypto_sign_ed25519_tweet_open`. `randombytes` застаблен
  (`abort()`) — keygen/sign в клиент не линкуются.
- Ключ: `kCatalogPublicKey[32]` в `catalog_service.cpp`, по умолчанию нули =
  подпись выключена (текущее поведение сохранено). Реальный ключ включает
  enforcement.
- Проверка в `refresh()` до `parseJson`/кэша: при включённом ключе тянет
  asset `*.sig` из того же trusted-release (`kTrustedReleasePrefix`),
  base64-декод → `verifyCatalogSignature` → `catalog_sig_verify`. Fail-closed:
  нет подписи / не сошлась → refresh абортит (MITM не срежет). Кэш и bundled
  локальны (доверенные при сборке / уже проверены при записи) — не
  перечекиваются.
- Продюсер: `tools/sign_catalog.py` (`--gen-key` печатает приватный hex +
  C-массив pubkey; `--key --sign` пишет base64 `.sig`-сайдкар). Живёт рядом с
  `embed_catalog_infodicts.py` в CI.
- Тест: `testCatalogSignatureVerify` (RFC 8032 вектор 2 + порча
  msg/sig/ключа). Полный конвейер проверен вживую: независимый подписант
  OpenSSL ed25519 → base64 → `catalog_sig_verify` = 1, порча = 0. golden +
  `tests/test_catalog` зелёные.

**Как включить:** `tools/sign_catalog.py --gen-key` → вставить pubkey в
`kCatalogPublicKey`, приватный ключ в CI-секрет, подписывать каждый релиз,
заливать `catalog.json.sig` рядом с `catalog.json`.

С добавлением зеркал/прокси растёт MITM-риск. Подписать `catalog.json`, зашить публичный ключ в бинарь, проверять в `parseJson` (`src/app/catalog_service.cpp:200`) до применения. Критично для П2.1 (встроенные info-dict).

**Почему Opus:** криптопроверка + управление ключами + встраивание в путь загрузки, но по устоявшемуся паттерну.

### П3.1 — Зеркала каталога ✅
**Оценка: M.**

**Сделано (2026-07-06):** `refresh()` в `src/app/catalog_service.cpp`
переписан как оркестратор источников:
1. `refreshFromGitHubRelease` — прежний путь (Releases API → asset + `.sig`).
2. `refreshFromMirror` для каждого из `kCatalogMirrors` (пока jsDelivr
   `cdn.jsdelivr.net/gh/bqio/switch-dumps@latest/catalog.json`; R2/IPFS
   добавляются одной строкой). Прямой GET, подпись из `url + ".sig"`.
3. Bundled-снапшот — конечный фолбэк, но он в `load()`, не в `refresh()`;
   refresh лишь возвращает объединённую ошибку, оставляя кэш/bundled в памяти.
- Общий `commitCatalog(body, signature?, label)` — единая точка verify (П3.2,
  fail-closed) + `parseJson` + `writeAtomic` + adopt для всех источников.
- Доверенный хост обобщён: `CatalogService::isTrustedSource(url)` (аллоулист
  префиксов github-releases + jsDelivr) заменил инлайновую github-проверку;
  каждый источник гейтится до фетча. Публичный статик → тестируемо.
- Тест: `testTrustedSourceAllowlist` (github/jsdelivr ok; http://, чужой репо,
  `github.com.evil.*`, пустая строка — reject). golden + `tests/test_catalog`
  зелёные.

**Как добавить зеркало:** строку URL в `kCatalogMirrors` + префикс хоста в
`isTrustedSource`. Зеркало должно отдавать тот же `catalog.json` и (при
включённой подписи) сайдкар `catalog.json.sig`.

Не полагаться на один GitHub Releases. Порядок попыток в `refresh()` (`src/app/catalog_service.cpp:308`):
1. GitHub Releases (как сейчас),
2. jsDelivr (`cdn.jsdelivr.net/gh/bqio/switch-dumps@latest/...`) — зеркалит GitHub, в РФ обычно жив,
3. Cloudflare R2 / Pages или IPFS-гейт,
4. bundled-снапшот в RomFS (уже есть, `load()` :297).

Обобщить валидацию источника (`assetUrl.rfind("https://github.com/bqio/...")`, :332) на список доверенных хостов.

**Почему Opus:** мульти-источниковый фолбэк + обобщение проверки доверенного хоста. Логика нетривиальная, но локализована в одном модуле.

### П1.2 — PEX-усиление тонкого списка пиров ✅
**Оценка: M.**

**Сделано (2026-07-06):** после tracker+DHT-фазы, если пиров ≤
`kPexThinThreshold` (3), резолвер коннектится к каждому и просит ut_pex
(`src/app/magnet_resolver.cpp`):
- `sendPexHandshake` — BEP10-хендшейк рекламит `ut_metadata`+`ut_pex` (id
  `kLocalUtPexId`=2), чтобы пир пушил свой рой на наш id.
- `harvestPexFromPeer` — читает extended-фреймы `kPexPeerTimeoutMs` (5 c),
  на нашем ut_pex-id декодит `added` (compact6), мержит в скретч через
  `appendUniquePeers`. Best-effort: PEX push по своему графику, пусто =
  норма.
- Врезка перед metadata-воркерами: харвест в отдельный вектор (аппенд в живой
  `peers` при итерации `peers.data()` дал бы dangling на реаллокации), затем
  один мерж и `[magnet] pex added N peers`. PEX-пиры дальше проходят обычную
  metadata-фазу и попадают в `verifiedPeers`, если сделали хендшейк.

Сборка golden + `tests/test_catalog` зелёные (регрессий нет).

BEP11 ut_pex в core уже есть. Когда прокси/DHT дали 1–3 пира, запросить у них ut_pex и дорастить набор через `appendUniquePeers`. Не бутстрапит с нуля (нужен первый пир), но дёшево умножает результат Фазы 0/1.

**Почему Opus:** протокольная обвязка внутри уже существующего пир-обмена, умеренная сложность.

---

## Тир Sonnet (простые)

### П0.2 — Фетч каталога через antizapret-прокси ✅
**Оценка: S.**

В `httpGet` каталога (`src/app/catalog_service.cpp:127`) добавить тот же путь, что в `game_metadata_service.cpp:242`: `antizapret_apply_route` + проверка `antizapret_response_looks_blocked` на теле. При желании добавить GitHub-домены в `host_is_target` (`src/core/antizapret.c:51`).

**Почему Sonnet:** копирование готового паттерна из соседнего файла.

### П0.3 — Тумблер / автодетект antizapret ✅
**Оценка: S.**

**Сделано (2026-07-06):** поле `useAntizapret` (default `true` = текущее
поведение) прокинуто через `AppSettingsData` +
`parseSettings`/`serializeSettings`/`operator==` (`use_antizapret` в JSON).
Точка применения `antizapret_set_enabled` в `main_switch.cpp:182` теперь читает
настройку вместо хардкода `1`. В настройках Switch — секция «Connectivity» с
тумблером «Antizapret bypass» (`settings_view.hpp`), меняет флаг и зовёт
`antizapret_set_enabled` вживую; `applyValues` синхронизирует движок с
настройкой. Тест `test_app_settings` расширен на новый флаг (дефолт + round-trip),
зелёный.

Исходное: `antizapret_set_enabled` уже есть,
`antizapret_note_proxy_success` (`src/core/antizapret.c:253`) реализует
«залипание» на прокси после первого успеха.

**Почему Sonnet:** пламбинг настроек по существующим паттернам `parseSettings`/`serializeSettings`.

### ПX.1 — Health-check через прокси в CI
**Оценка: S.**

`tools/check_catalog_health.py` уже поддерживает `--proxy` (:127). При прогоне из РФ-CI гонять с прокси, иначе живые раздачи пометятся мёртвыми. Обвязка CI, кода почти нет.

**Почему Sonnet:** флаг уже есть, задача — конфиг CI.

### ПX.2 — Публичные open-трекеры в кандидаты
**Оценка: S.**

Добавить `opentrackr`, `open.stealth.si` и др. в `rutrackerTrackerCandidates` (`src/app/magnet_resolver.cpp:86`) и разрешить в `allowedTracker` (:74). Низкий хит-рейт (рой RuTracker там редко анонсится) — брать только вместе с DHT.

**Почему Sonnet:** добавление строк в список + белый список хостов.

### ПX.3 — Телеметрия источника резолва
**Оценка: S.**

Логировать сработавший источник (direct/proxy/route-name/DHT/зеркало) и долю успешных резолвов. Каркас логов уже есть (`[magnet]`, `[antizapret]`, `[catalog]`).

**Почему Sonnet:** добавление log-строк по существующему каркасу.
