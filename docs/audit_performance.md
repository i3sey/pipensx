# Аудит производительности pipensx

Дата: 2026-10-26. База: `e89af6b227017c1c826d805b9192c5f8802278d1`.
Ветка: `perf/overnight-optimizations`, создана от текущей `main`.

## Метод и границы выводов

Это статический аудит с проверкой сборок. Ниже приведены гипотезы, а не
измеренные на Switch ускорения. Impact означает ожидаемое влияние при
указанном сценарии, Risk — риск изменения поведения. Замеры CPU, RSS,
задержек SD и p95/p99 кадров на устройстве пока отсутствуют. Формат состояния,
сетевые протоколы, install/platform и vendor не изменялись.

## Архитектура и потоки

- `src/core` — C11: bencode/metainfo, peer, torrent, DHT, хэширование и pieces.
  `torrent_tick` (`src/core/torrent.c:2068`) обслуживает сеть, результаты
  хэширования и проверку существующих pieces. `poll(..., 10)` находится на
  строке 2262. Стартовая проверка ограничивается временным бюджетом между
  pieces; один медленный read/hash может превысить этот бюджет.
- `DownloadManager` (`src/app/download_manager.cpp:531`) запускает persistence
  и scheduler workers, отдельные runners задач (`:2721`), cleanup worker.
  Сетевой tick выполняется вне manager mutex (`:3249`), публикация прогресса
  под ним (`:3298`). `snapshotUi` (`:1799`) копирует облегчённые задачи,
  `snapshot` (`:1786`) — полные. Persistence уже имеет отдельный mutex/worker;
  радикальная переделка здесь не нужна без измерений.
- Web seed имеет очередь pending/completed и workers; fetch вынесен из mutex
  (`src/app/web_seed_source.cpp:179`). Данные передаются перемещением.
- Каталог и metadata представлены snapshots; browse работает через
  `brls::async`, результат применяется через `brls::sync`
  (`src/ui/catalog/catalog_view.hpp:630`). Generation/cancellation и одна
  pending browse job предотвращают очередь устаревших поисков.
- Artwork: local worker + network workers
  (`src/app/game_metadata_service.cpp:626`), decoded RAM cache, файловый
  cache, request deduplication и retry. UI получает готовый результат.
- Downloads UI (`src/ui/downloads/downloads_view.hpp:519`) опрашивает
  `snapshotUi`, различает Reload/Repaint/отсутствие изменений; обновляет
  видимые cells и откладывает reload при overlay. Recycler уже используется.
- Upload текстур имеет бюджет байт и времени на кадр
  (`src/ui/common/image_upload_budget.hpp:15`). Это существующая защита;
  её нельзя считать отсутствующей или менять вслепую.

## Карта узких мест и приоритеты

| ID | Место | Наблюдение и сценарий | Impact | Risk | Следующий шаг |
|---|---|---|---|---|---|
| P1 | `src/app/game_metadata_service.cpp:1688–1730` | При попадании в cache более крупного изображения `sizedRgba` и создание thumbnail выполняются под imageMutex; UI requestImage использует тот же mutex (:1304) | High | Med | Удержать shared_ptr source под lock, resize выполнить после unlock; сохранить touch и проверку актуальности при публикации |
| P2 | `src/app/catalog_presentation.cpp:335,352` | Одна и та же классификация section вызывается дважды для записи; классификация может копировать и приводить title/package к нижнему регистру (`catalog_section.hpp:41`) | Med | Low | Вычислить один bool на запись и использовать для genres и фильтра; существующие browse/section tests + сравнение результатов |
| P3 | `src/app/catalog_presentation.cpp:345` | foldAscii(infoHash) создаёт строку для каждой записи даже при отключённом favoritesOnly | Low | Low | Перенести folding внутрь проверки favoritesOnly; не менять нормализацию ключей результата |
| P4 | `src/app/catalog_presentation.cpp:380–392` | Alphabetical sort: unordered_map<int,string>, узел на запись, два hash lookup на сравнение | Med | Med | Проверить плотный массив ключей/индексов; сравнить peak RSS при сильном фильтре и стабильность равных ключей |
| P5 | `src/app/catalog_presentation.cpp:279–283` | Для каждой строки каждого поля поиска создаётся folded string | Med | Med | Кэш folded полей по snapshot generation с лимитом памяти либо потоковый matcher; сохранить кириллицу, malformed UTF-8 и cancellation |
| P6 | `src/app/catalog_service.cpp:488–501` | Полный JSON DOM одновременно с входной строкой и итоговыми CatalogEntry; std::set hashes имеет node allocation на уникальный hash | High (peak memory) | Med | Сначала unordered_set+reserve с duplicate fixtures; отдельно SAX после измерения пика памяти |
| P7 | `src/app/game_metadata_service.cpp:667,806–836` | DOM metadata, копия indexJson в indexData; cached read дополнительно держит vector и string | High (peak memory) | Med | Уточнить владение raw bytes, передавать один буфер без потери данных для проверки manifest/hash и сохранения |
| P8 | `src/ui/downloads/downloads_view.hpp:573–578`; `downloads_view.cpp:6–59` | На progress repaint копируется tasks_ в datasource, заново строятся sections пятью проходами | Med | Med | Разделить обновление значений и структуры; проверить focus/overlay/deploy и соответствие индексов |
| P9 | `src/app/download_manager.cpp:1799–1805` | Каждый UI snapshot аллоцирует vector и строки под общим mutex; стоимость зависит от числа задач | Med | Med | Сначала длительность snapshot/lock и число задач; затем immutable UI snapshot с generation, без атомизации отдельных несогласованных полей |
| P10 | `src/app/web_seed_source.cpp:205–242` | На каждый piece создаётся/удаляется CURL handle; теряется возможность переиспользовать соединение между pieces | High (HTTP latency) | Med | Один handle на worker; reset state/callback pointers, redirects/range/errors/stop tests на локальном HTTP fixture |
| P11 | `src/app/download_manager.cpp:3266–3283` | При заполненном/завершённом web seed возможны повторные проходы по всем pieces | Med | Med | Измерить enqueue probes на tick; ограниченная очередь кандидатов с корректной invalidation/retry |
| P12 | `src/core/peer.c:70,663,711` | Компактизация send/receive buffers через memmove | Med | High | Измерить bytes moved; ring/offset модель затрагивает framing, encryption и uTP |
| P13 | `src/core/bencode.c:75–163` | be_decode сканирует вложенный контейнер, последующий обход повторяет scan | Low/Med | High | Профиль на реальных metainfo; сохранить validation, глубину, raw ranges и malformed input поведение |
| P14 | `src/core/piece.c:362–364` | Bitmap/request_counts выделяются для каждого piece при инициализации | Med (startup/RSS) | High | Ленивые allocations только после измерения; проверить endgame/reset/cancel/worker lifetime |
| P15 | `src/app/catalog_descriptions.cpp:79–94`; `src/ui/detail/game_detail.hpp:130` | Открытие detail вызывает offline description lookup: fopen/fread/zstd выполняются синхронно под общим mutex; cache содержит один block | High (cold detail) | Med | Подготовка текста в worker с locale/generation guard; immutable block cache с ограничением RAM |
| P16 | `src/app/download_manager.cpp:2208–2215` | Persistence I/O вне manager mutex, но сериализация полной очереди под ним; большие selections/bitfields задерживают snapshotUi | Med | Med | Измерить serialize duration/bytes/lock wait; перенос сериализации требует ограниченного snapshot и контроля generation |
| B1 | `CMakeLists.txt:483` | Каждый configure удаляет romfs/catalog; make switch всегда configure, затем снова сжимает snapshots zstd -15 | High (incremental build) | Med | Заменить очистку на удаление строго устаревших outputs; проверить смену входного пути, отключение metadata и отсутствие старого dataset в NRO |
| B2 | `Makefile.pc:107` и правила objects | Нет -MMD/-MP и include dependency files: header edit может оставить stale objects | High (валидность проверок) | Med | Внедрять отдельно с проверкой touch header, dual builds; время no-op без header deps не доказывает корректность |

## Уже реализованные оптимизации, которые не следует дублировать

- Pool piece buffers (`src/core/piece.c:77–97`), reuse verify_buf (:531–535).
- Off-thread hash results drain (`src/core/torrent.c:2091`), async tracker
  reannounce (:2181); не переносить протоколы в новые потоки автоматически.
- reserve каталога и metadata indexes (`catalog_service.cpp:501`,
  `game_metadata_service.cpp:844,853–854`), move результата распаковки
  (`src/app/snapshot_zstd.cpp:58,70`).
- Лимит распакованного размера snapshot и проверка frame header уже есть.

## Сборочные флаги

`Makefile.pc:12–24`: -O2, C11/C++17, assertions для C++; sanitizer opt-in.
`CMakeLists.txt:26–32`: Switch Release -O2 -DNDEBUG и IPO/LTO уже включены.
`scripts/build_switch.sh:44`: Release выбран явно. Автоматическое включение
-O3 или архитектурных расширений не обосновано: нужны сравнения времени,
размера, RSS и аппаратная проверка. Наличие crypto extension само по себе
не делает текущий SHA код аппаратным. Настройки vendor не менялись.

## Протокол измерений после восстановления полной проверки

Для каждой отдельной правки: полный `make -f Makefile.pc test && make switch
&& make pc`, затем атомарный perf-коммит. При любой ошибке восстановить
изменённый код, записать причину; тесты не ослаблять. C++20 в shared code
не использовать. Для header изменений сначала исключить stale objects.

Для runtime: одинаковый snapshot, query/filters, число задач и artwork;
сравнить median/p95/p99 buildCatalogBrowse, snapshotUi, refresh и кадра,
peak heap, lock hold/wait, piece tick latency, HTTP connections/piece.
Для Switch SD прогнать cold/warm cache отдельно. Сетевые тесты не заменять
таймингом локального хоста. Для incremental build сравнить первый build,
no-op, изменение одного .cpp и header; отдельно configure/compress/link.

## Итог и журнал проверки

Начальные команды на хосте завершились до внесения изменений:
- `make -f Makefile.pc test`: ошибка отсутствующего curl/curl.h (2,26 с).
- `make switch`: DEVKITPRO не задан (0,00 с).
- `make pc`: ошибка отсутствующего curl/curl.h (1,10 с).

Найдены уже установленные Docker-образы `catthehacker/ubuntu:act-24.04`
(PC) и `devkitpro/devkita64:latest` (Switch). zstd установлен только во
временном Switch-контейнере. Первый запуск Switch с mount по исходному
пути выявил cache от /work; mount исправлен на /work без изменения проекта.
Switch baseline: PASS, 6,110 с. Повторный no-source-change `make switch`:
PASS, 5,544 с, оба JSON.zst снова генерируются. Это warm build с ранее
существовавшими артефактами, не чистая сборка. Установка пакетов и старт
контейнера в эти времена не входят. Runtime ускорения этими числами не
измеряются. Полные логи сессии: `/tmp/pipensx-perf/` (вне git).

## High Risk: оставить человеку

1. Receive/send ring buffers (P12): меньше memmove на высоком throughput,
   но меняются lifetime указателей и границы сообщений. Внедрять после
   счётчиков копирования, fragmentation/coalescing/reconnect fixtures,
   fuzzing encrypted/plaintext peer streams и аппаратного soak test uTP.
2. Индексированный/однопроходный bencode (P13): убирает повторные scan,
   но malformed packets и raw info hash должны оставаться побайтно
   совместимыми. Differential corpus/fuzz tests, вложенность и overflow,
   затем двойная сборка и реальная metadata/DHT нагрузка.
3. Lazy piece metadata (P14): экономит allocations и память больших
   торрентов. Сначала определить ownership и thread synchronization,
   затем failure injection, reset/endgame/cancellation и ASan/TSan на PC.
4. Замена manager mutex атомиками/lock-free snapshots: может уменьшить UI
   wait, но раздельные counters дают несовместимые progress/status snapshots,
   а reclamation требует lifetime-модели. Сначала p99 mutex wait, затем
   immutable publication под узкой блокировкой, stress tests удаления и
   перезапуска задач; load/save оставить совместимыми.

Baseline PC в контейнере: первый полный прогон остановился на
`tests/test_port_archive.cpp:57`: отсутствовала команда `7z`, assert при
создании fixture; 4m30,791s включая компиляцию. Подготовленная P2 немедленно
откачена через `git checkout -- src/app/catalog_presentation.cpp`; коммитов
не было. В отдельный тестовый контейнер установлен p7zip-full, baseline
перезапущен. PC CLI: PASS, 0,150 с (компиляция main + link), повторный
no-op `make pc`: PASS, 0,003 с. Эти времена не включают запуск Docker.

Повтор baseline после p7zip: все ранние native tests, Python tests и i18n
прошли; последний `test_manager` упал на `tests/test_manager.cpp:1257`:
`reloaded.snapshot()[0].resumeBitfield == std::vector<uint8_t>(1, 0)`.
Длительность 1m38,215s. К этому моменту исходники полностью восстановлены;
это падение baseline, не доказательство регрессии оптимизации. Тест ожидает
сохранённый fast-resume bitfield после pause/shutdown. Причина пока не
установлена; объявлять её сетевой нельзя. Редактирование load/save и
ослабление assert не выполнялись. Запущен повтор полного baseline для
проверки воспроизводимости.

Второй полный неизменённый baseline повторил assert `test_manager.cpp:1257`
за 1m32,537s. Запущена чистая пересборка PC (`make -f Makefile.pc clean`
перед test), чтобы исключить stale objects. В обоих прогонах прошли 67
ранних native binaries; это не означает успешного полного suite.

### Финальный статус

Чистая пересборка (`make -f Makefile.pc clean`, затем
`MAKEFLAGS=-j4 make -f Makefile.pc test` в PC-контейнере с curl/crypto/zstd/zlib
и p7zip-full) снова воспроизвела **тот же assert на строке 1257**.
Длительность: 2m26,956s, включая полную компиляцию. C++17-компиляция прошла;
полный suite — FAIL. Ранее полученный результат не объясняется stale objects.
После чистой проверки `make pc` и повторный no-op `make pc` снова PASS.
Switch NRO baseline и повторная инкрементальная сборка — PASS.

- Успешно внесённых и полностью проверенных оптимизаций: **0**.
- Коммитов: **0**; ссылки на perf-коммиты отсутствуют. Запрет коммитов при
  красном обязательном suite применён также к документации.
- P2 подготовлена и откачена; остальные runtime/build правки не вносились.
- Единственный оставленный файл — этот отчёт, незакоммиченный.
- Все 16 runtime-гипотез и две сборочные задачи остаются кандидатами;
  P1/P2/P3 — первые после восстановления baseline. High Risk перечислены выше.
- Временные созданные контейнеры и производный PC image удалены; исходные
  Docker images, исходники и vendor сохранены. Логи остаются в /tmp/pipensx-perf.

### Блокер для следующего прохода

Нужен отдельный разбор fast-resume инварианта: после `pause` in-memory
bitfield уже непустой (assert armed проходит), но после `shutdown` и нового
`DownloadManager` значение отличается от ожидаемого `{0}`. Проверить
фактические bytes queue.bencode, serializer `:2056`, generation ordering
persistence worker `:2189`, teardown `:3491` и parser `:2345` в
`src/app/download_manager.cpp`. Сохранить бинарный fixture и лог generation
до/после shutdown; различить неверную запись, загрузку и ожидание теста.
Причина пока не доказана. Не менять протокол/формат, не добавлять sleep ради
зелёного результата, не ослаблять assert. В текущем аудите persistence
автоматически не исправлялся ввиду явного ограничения на load/save и
отсутствия успешного baseline. После отдельного исправления повторить все
три обязательные команды, затем внедрять кандидатов по одному.
