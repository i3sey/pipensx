const i18n = {
  en: {
    navFeatures: "Features",
    navScreenshots: "Screenshots",
    navInstall: "Installation",
    navSpecs: "Specs",
    heroTag: "NINTENDO SWITCH HOMEBREW",
    heroHeading: "BitTorrent client & package installer for Nintendo Switch",
    heroLead: "Download games, updates, DLC and fan ports directly on your console. Installs NSP, NSZ and XCI packages on the fly without needing twice the free storage space.",
    btnDownload: "Download pipensx.nro",
    btnGitLab: "GitLab Repository",
    metaCFW: "CFW: Atmosphère",
    metaArch: "C11 / C++20",
    metaUI: "Borealis 60 FPS",
    bannerCaption: "Downloads view: Active torrent transfers and categories",
    featuresLabel: "CAPABILITIES",
    featuresTitle: "Engineered specifically for Nintendo Switch",
    featStreamTitle: "Direct Streaming Installation",
    featStreamDesc: "Streams package data directly into NAND or SD card storage while downloading. Eliminates the requirement to keep 30 GB free for the archive plus 30 GB for the installed game.",
    featDebridTitle: "Debrid & HTTP Acceleration",
    featDebridDesc: "Native support for TorBox, TorrServer, Real-Debrid, and AllDebrid. Downloads cached torrents over encrypted HTTPS connections at full line speed.",
    featTorrentTitle: "Embedded BitTorrent Engine",
    featTorrentDesc: "Complete standalone C11 BitTorrent core with libutp, mainline DHT, tracker scrape, piece integrity hashing, and optimized SD card write buffering.",
    featLibraryTitle: "Installed Titles & Port Manager",
    featLibraryDesc: "Inspect installed games, base builds, DLCs, and version numbers. Automatically detects PC game ports and deploys LayeredFS game files.",
    featBorealisTitle: "Native Controller Interface",
    featBorealisDesc: "Fluid 60 FPS interface built on Borealis. Full support for Joy-Cons, Pro Controllers, D-Pad, analog stick scrolling, and capacitive touchscreen.",
    featSecurityTitle: "Open & Audited Source",
    featSecurityDesc: "No telemetry, no hidden accounts, and zero proprietary trackers. Source code is open and scanned via GitLab Secret Detection and SAST.",
    screensLabel: "INTERFACE",
    screensTitle: "Actual Nintendo Switch captures",
    screen1Title: "Game Catalog & Navigation",
    screen1Desc: "Fast catalog grid with cover art, release details, genres, and instant search.",
    screen2Title: "Details & Versions",
    screen2Desc: "Inspect version numbers, descriptions, screenshots, and choose download sources.",
    screen3Title: "Active Downloads Queue",
    screen3Desc: "Track transfer speed, peer count, ETA, download categories, and installation progress.",
    screen4Title: "Installed Titles & Management",
    screen4Desc: "Direct sidebar actions: view updates, inspect DLC, uninstall files, and launch games.",
    installLabel: "GETTING STARTED",
    installTitle: "Installation Guide",
    tabDBI: "Method 1: DBI (USB-C)",
    tabSD: "Method 2: MicroSD Card",
    dbiStep1Title: "Connect your Nintendo Switch",
    dbiStep1Desc: "Plug your Switch into your computer using a standard USB-C data cable.",
    dbiStep2Title: "Start MTP Responder in DBI",
    dbiStep2Desc: "Open DBI from the Homebrew Menu and select \"Run MTP Responder\".",
    dbiStep3Title: "Open Switch storage on computer",
    dbiStep3Desc: "Open the mounted \"Switch\" storage drive on your PC and navigate to \"1: External SD Card\" -> \"switch\".",
    dbiStep4Title: "Copy pipensx.nro",
    dbiStep4Desc: "Create folder \"pipensx\" and place pipensx.nro into it: /switch/pipensx/pipensx.nro.",
    dbiStep5Title: "Exit MTP Responder",
    dbiStep5Desc: "Press [X] on your Nintendo Switch to stop MTP Responder. pipensx will now appear in your Homebrew Menu.",
    sdStep1Title: "Turn off console or open Hekate UMS",
    sdStep1Desc: "Safely power off your Nintendo Switch and remove the microSD card, or boot into Hekate and choose Tools -> USB Tools -> SD Card.",
    sdStep2Title: "Insert card into PC card reader",
    sdStep2Desc: "Connect the microSD card to your computer via card reader.",
    sdStep3Title: "Copy executable to SD card",
    sdStep3Desc: "Copy pipensx.nro into the switch folder: SD:/switch/pipensx/pipensx.nro.",
    sdStep4Title: "Insert card back and boot CFW",
    sdStep4Desc: "Reinsert the card into the console and boot into Atmosphère CFW.",
    alertTitle: "Important: Launch via Title Override (Full RAM Mode)",
    alertDesc: "Do NOT launch pipensx from the Album icon (Applet Mode). Applet mode restricts memory to ~400 MB, causing out-of-memory errors during torrent decompression. Hold [R] while launching any installed game on the Switch home screen to launch the Homebrew Menu in full RAM mode (3.2+ GB).",
    specsLabel: "TECHNICAL DETAILS",
    specsTitle: "System Requirements",
    specPlatform: "Target Platform",
    specPlatformVal: "Nintendo Switch (V1, V2, Lite, OLED) running Atmosphère CFW",
    specFormat: "Executable Format",
    specFormatVal: "Homebrew NRO (.nro)",
    specMemory: "Memory Ceiling",
    specMemoryVal: "Title Override mode recommended (3.2 GB RAM available)",
    specPackages: "Supported Packages",
    specPackagesVal: "NSP, NSZ (Zstandard compression), XCI",
    specProtocols: "Network Protocols",
    specProtocolsVal: "BitTorrent, uTP, DHT, HTTPS (TorBox, TorrServer, Real-Debrid)",
    footerText: "pipensx is open-source software built for Nintendo Switch homebrew enthusiasts.",
    footerReleases: "Releases",
    footerGitLab: "GitLab",
    footerDocs: "Documentation"
  },
  ru: {
    navFeatures: "Возможности",
    navScreenshots: "Скриншоты",
    navInstall: "Установка",
    navSpecs: "Характеристики",
    heroTag: "NINTENDO SWITCH HOMEBREW",
    heroHeading: "BitTorrent-клиент и установщик пакетов для Nintendo Switch",
    heroLead: "Загрузка игр, обновлений, DLC и фанатских портов прямо на консоли. Потоковая установка пакетов NSP, NSZ и XCI без необходимости держать на карте памяти двойной объем свободного места.",
    btnDownload: "Скачать pipensx.nro",
    btnGitLab: "Репозиторий GitLab",
    metaCFW: "CFW: Atmosphère",
    metaArch: "C11 / C++20",
    metaUI: "Borealis 60 FPS",
    bannerCaption: "Экран загрузок: активные раздачи, фильтры и статус потоковой установки",
    featuresLabel: "ВОЗМОЖНОСТИ",
    featuresTitle: "Разработано специально для Nintendo Switch",
    featStreamTitle: "Потоковая установка без двойного места",
    featStreamDesc: "Пакет устанавливается в память NAND или на SD-карту прямо в процессе скачивания торрента. Больше не нужно иметь 30 ГБ под архив плюс 30 ГБ под установленную игру.",
    featDebridTitle: "Поддержка Debrid и ускорение HTTP",
    featDebridDesc: "Нативная интеграция с TorBox, TorrServer, Real-Debrid и AllDebrid. Скачивание кэшированных торрентов по защищенному HTTPS на максимальной скорости интернет-канала.",
    featTorrentTitle: "Встроенный BitTorrent-движок",
    featTorrentDesc: "Автономное ядро BitTorrent на C11: протокол libutp, поддержка DHT (бестрекерный поиск), опрос трекеров, проверка контрольных сумм и буферизация записи на SD.",
    featLibraryTitle: "Управление библиотекой и портами",
    featLibraryDesc: "Просмотр установленных игр, базовых версий, патчей и дополнений. Автоматическое распознавание ПК-портов и копирование файлов LayeredFS в каталог Atmosphère.",
    featBorealisTitle: "Интерфейс под геймпад и тачскрин",
    featBorealisDesc: "Плавный интерфейс 60 кадров/сек на базе Borealis. Полное управление с Joy-Con, Pro Controller, стиков, крестовины и емкостного сенсорного экрана.",
    featSecurityTitle: "Открытый и проверяемый код",
    featSecurityDesc: "Никакой телеметрии, скрытых сервисов и проприетарных трекеров. Исходный код открыт и проверяется статическим анализатором SAST и GitLab Secret Detection.",
    screensLabel: "ИНТЕРФЕЙС",
    screensTitle: "Реальные снимки экрана с Nintendo Switch",
    screen1Title: "Каталог игр и поиск",
    screen1Desc: "Удобная сетка с обложками, фильтрацией по жанрам и моментальным поиском.",
    screen2Title: "Карточка игры и выбор версий",
    screen2Desc: "Подробное описание, проверка доступных патчей, скриншоты и выбор источников загрузки.",
    screen3Title: "Очередь активных загрузок",
    screen3Desc: "Отображение скорости, количества сидов/пиров, расчет оставшегося времени и категории.",
    screen4Title: "Установленные игры и управление",
    screen4Desc: "Боковая панель действий: проверка обновлений, DLC, удаление и запуск игр.",
    installLabel: "РУКОВОДСТВО",
    installTitle: "Инструкция по установке",
    tabDBI: "Способ 1: Через DBI (USB-C)",
    tabSD: "Способ 2: Напрямую на MicroSD",
    dbiStep1Title: "Подключите Nintendo Switch к компьютеру",
    dbiStep1Desc: "Соедините консоль с компьютером через обычный USB-C кабель передачи данных.",
    dbiStep2Title: "Запустите MTP Responder в DBI",
    dbiStep2Desc: "Откройте DBI в Homebrew Menu на Switch и выберите пункт «Run MTP Responder».",
    dbiStep3Title: "Откройте накопитель Switch на компьютере",
    dbiStep3Desc: "В проводнике откройте появившийся диск «Switch», перейдите в «1: External SD Card» -> «switch».",
    dbiStep4Title: "Скопируйте pipensx.nro",
    dbiStep4Desc: "Создайте папку «pipensx» и скопируйте туда файл: /switch/pipensx/pipensx.nro.",
    dbiStep5Title: "Завершите сеанс MTP",
    dbiStep5Desc: "Нажмите кнопку [X] на консоли, чтобы отключить MTP. pipensx появится в вашем Homebrew Menu.",
    sdStep1Title: "Выключите консоль или откройте Hekate UMS",
    sdStep1Desc: "Полностью выключите Switch и извлеките карту памяти (либо в Hekate выберите Tools -> USB Tools -> SD Card).",
    sdStep2Title: "Вставьте карту памяти в кардридер ПК",
    sdStep2Desc: "Подключите microSD к компьютеру через USB-адаптер или кардридер.",
    sdStep3Title: "Скопируйте исполняемый файл",
    sdStep3Desc: "Скопируйте pipensx.nro в каталог switch: SD:/switch/pipensx/pipensx.nro.",
    sdStep4Title: "Установите карту обратно и запустите CFW",
    sdStep4Desc: "Вставьте microSD обратно в консоль и запустите прошивку Atmosphère.",
    alertTitle: "Важно: Запуск через Title Override (Full RAM Mode)",
    alertDesc: "Не запускайте pipensx через иконку «Альбом» (Applet Mode). В режиме апплета консоль выделяет приложению всего ~400 МБ памяти, что вызывает ошибку нехватки RAM при распаковке торрента. Зажмите клавишу [R] на геймпаде и запустите ЛЮБУЮ установленную игру из главного меню Switch, чтобы войти в Homebrew Menu с доступом ко всей оперативной памяти (3.2+ ГБ).",
    specsLabel: "ТЕХНИЧЕСКИЕ ДАННЫЕ",
    specsTitle: "Системные требования",
    specPlatform: "Платформа",
    specPlatformVal: "Nintendo Switch (V1, V2, Lite, OLED) с прошивкой Atmosphère CFW",
    specFormat: "Формат файла",
    specFormatVal: "Homebrew NRO (.nro)",
    specMemory: "Выделение памяти",
    specMemoryVal: "Режим Title Override (доступно 3.2 ГБ оперативной памяти)",
    specPackages: "Поддерживаемые форматы",
    specPackagesVal: "NSP, NSZ (сжатие Zstandard), XCI",
    specProtocols: "Сетевые протоколы",
    specProtocolsVal: "BitTorrent, uTP, DHT, HTTPS (TorBox, TorrServer, Real-Debrid)",
    footerText: "pipensx — открытое программное обеспечение для энтузиастов Nintendo Switch homebrew.",
    footerReleases: "Релизы",
    footerGitLab: "GitLab",
    footerDocs: "Документация"
  }
};

let currentLang = "en";

function setLanguage(lang) {
  if (!i18n[lang]) return;
  currentLang = lang;
  localStorage.setItem("pipensx_lang", lang);
  document.documentElement.lang = lang;

  document.querySelectorAll("[data-i18n]").forEach((el) => {
    const key = el.getAttribute("data-i18n");
    if (i18n[lang][key]) {
      el.textContent = i18n[lang][key];
    }
  });

  document.querySelectorAll(".lang-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.getAttribute("data-lang") === lang);
  });
}

function initTabs() {
  const tabBtns = document.querySelectorAll(".tab-btn");
  const tabPanels = document.querySelectorAll(".guide-panel");

  tabBtns.forEach((btn) => {
    btn.addEventListener("click", () => {
      const targetId = btn.getAttribute("data-tab");
      tabBtns.forEach((b) => b.classList.remove("active"));
      tabPanels.forEach((p) => p.classList.remove("active"));

      btn.classList.add("active");
      const targetPanel = document.getElementById(targetId);
      if (targetPanel) targetPanel.classList.add("active");
    });
  });
}

function initModal() {
  const modal = document.getElementById("img-modal");
  const modalImg = document.getElementById("modal-img");
  const closeBtn = document.getElementById("modal-close");

  function openModal(src, alt) {
    modalImg.src = src;
    modalImg.alt = alt || "Screenshot";
    modal.classList.add("active");
    document.body.style.overflow = "hidden";
  }

  function closeModal() {
    modal.classList.remove("active");
    document.body.style.overflow = "";
  }

  document.querySelectorAll(".zoomable").forEach((el) => {
    el.addEventListener("click", () => {
      const img = el.tagName === "IMG" ? el : el.querySelector("img");
      if (img) openModal(img.src, img.alt);
    });
  });

  closeBtn.addEventListener("click", closeModal);
  modal.addEventListener("click", (e) => {
    if (e.target === modal) closeModal();
  });

  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && modal.classList.contains("active")) {
      closeModal();
    }
  });
}

document.addEventListener("DOMContentLoaded", () => {
  const saved = localStorage.getItem("pipensx_lang");
  const browserLang = (navigator.language || "").startsWith("ru") ? "ru" : "en";
  setLanguage(saved || browserLang);

  document.querySelectorAll(".lang-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      setLanguage(btn.getAttribute("data-lang"));
    });
  });

  initTabs();
  initModal();
});
