# Независимая техническая проверка macOS-порта LMZ Signal 2M 0.3.1

Дата обновления: 27 октября 2026 года, Europe/Moscow. Итог: **нативные автоматизированные технические проверки и упаковка PASS** на Apple Silicon и Intel в [GitHub Actions run 40419330616](https://github.com/Onznaet/signal2m_macos/actions/runs/40419330616), commit `c3dc36f91ade3c6e5194ffceccd5a009faeab2ae`. Проверен фактический скачанный Universal VST3, а не только исходники. Визуальное поведение и работа в DAW этим заключением не подтверждаются.

## Подтверждённая нативная проверка

В run5 каждый runner собрал bundle с arm64 и x86_64, выполнил все девять CTest в своей нативной архитектуре, подписал доставку и проверил ZIP после распаковки. ARM64 CTest: **9/9 PASS за 9,35 с**; Intel CTest: **9/9 PASS за 12,76 с**. Ранее run4 также завершил девять тестов успешно на обеих архитектурах, но остановился на ошибке текстового парсера `otool` в упаковщике; версия linker tool ошибочно считалась minimum OS. Эта ошибка устранена с сохранением строгих требований к ОС и архитектурам.

Независимый VST3-хост проверил настоящий bundle через CoreFoundation, `bundleEntry`, `GetPluginFactory` и `bundleExit`: **1422 assertions, 0 failures**. Он повторно выполнен на bundle, восстановленном из фактически создаваемого ZIP: arm64 на ARM64 runner, x86_64 на Intel runner. Два разных CI-артефакта не объявляются побайтно одинаковыми; manifest каждого указывает архитектуру своего фактического исполнения. Тесты шли с Apple clang 17, Xcode 16.4 и macOS SDK 15.5.

Сценарии покрывают одиннадцать стабильных ID панели, автоматизацию, MIDI с ненулевыми sample offsets, ноту 60 как нейтральную высоту, повторные и перекрывающиеся ноты, MIDI-каналы, CC120/123, смену транспорта, разные размеры блоков, моно/стерео, конечное затухание, длительную Oscillation и независимые экземпляры. Проверены сохранение состояния и чтение настоящего 616-байтового fixture версии 0.1. Редактор и DAW для этих проверок не открывались.

## Идентичность и доставка

Подтверждены версия `0.3.1`, имя хосту `LMZ Signal 2M`, производитель `LMZ`, физический bundle `Signal2M.vst3`, bundle ID `sound.lmz.signal2m`, коды `Lmzz`/`Sg2m` и прежние processor/controller/compatibility FUID. Сравнение семантических FUID учитывает платформенное представление TUID.

Основной ZIP в `dist-macos/Signal2M-v0.3.1-macOS-Universal.zip` получен из ARM64 job, содержит обе архитектуры, размер **5 767 813 байт**. Его SHA-256:

`d916a9fdbc52578769a74ad1e58ee8f5faa7c57dc767548344f4e6ed28e9aedf`

SHA-256 подписанного executable внутри: `3c748763a2c51bb2ff1e849dddc9d46fd86a23859be83e79dc469b051be47fca`. Этот же хеш записан для исходного протестированного и доставленного подписанного executable.

Параллельный Universal ZIP Intel job, сохранённый под `build/github-publish/run5-x86_64/native/dist-macos`, имеет размер **5 767 812 байт** и SHA-256 `5705d4f62db7cefb7088109a1b0ca99d39e911b2cb511540dcc5a6a18d62e3d0`. Его executable: `b5bc08d8ce119764914ad35db6e96fa3c6a2e094372dbff2696b478adeedc0d9`.

Оба архива независимо проверены локальным `ReviewerMacPackageChecks.py --offline`. Прямой разбор Mach-O подтвердил arm64/x86_64, реальные minimum OS **11.0/10.15**, корректные границы структур, системные Apple-зависимости и отсутствие Windows runtime. Пройдены plist/moduleinfo, классы, endpoint, ZIP CRC, sidecar SHA-256, manifest, права исполнения и точное совпадение файлов извлечённого bundle с архивом. Полные нативные CI-логи дополнительно подтверждают `codesign --verify --deep --strict --all-architectures` для staging и распакованного ZIP, затем успешный host probe и нативный package reviewer. Локальный offline-режим сам не подменяет эту проверку подписи.

## Обновления и время жизни

Mac читает только A1:B1 вкладки `gid=640439888` таблицы `1ZyDHZskIvieqR_L6y4Ghu0gwvc6usByiNUF52y6AidM`; Windows сохраняет `gid=0`. A1 задаёт версию, B1 — страницу перехода. Автоматической загрузки B1 и записи в таблицу нет.

Нативные контролируемые HTTP/TLS-сценарии подтвердили получение CSV, перенаправления, body bounds, анонимность cookies/auth, отмену и общий deadline. Сетевой лимит — восемь секунд, отдельный cleanup budget — до 250 мс. TLS-доверие и проверка имени хоста переданы ОС без обхода проверки сертификатов.

Диагностика штатного fetch на обеих архитектурах показала invalidation, пустую очередь, отсутствие активных callbacks и уничтоженный delegate; contexts, runtime-классы, временные pins и fallbacks равны нулю. При этом системные session/queue ещё существовали: их деаллокация не равна завершению callback lifetime. Это соответствует [контракту Apple invalidation](https://developer.apple.com/documentation/foundation/urlsession/invalidateandcancel%28%29).

Отдельный loader без JUCE в основном executable проверил динамический dylib, отмену, deadline и аварийный pin/circuit breaker. Принудительный fallback доступен только тестовому fixture и не имеет reset/unpin. В run5 выполнены 29 штатных `dlclose` на каждом runner; максимальная измеренная деструкция checker — 25,14 мс ARM64 и 39,45 мс Intel. Образы оставались резидентными после этих вызовов: фактическое unmapping не наблюдалось и не заявляется.

## Границы заключения

Подпись **ad-hoc** успешно проверена, но Developer ID и notarization не выполнялись. Runtime на минимальных macOS 10.15/11.0, DAW scan, интерфейс, открытие/закрытие редактора и пользовательский старый проект остаются ручной приёмкой. Отдельный внешний probe с недоверенным TLS-сертификатом не запускался и не включён в PASS.

Нативные доказательства находятся в `build/github-publish/run5-arm64` и `run5-x86_64`: `diagnostics/github-actions-build.log`, `Testing/Temporary/LastTest.log`, `native-test-results.json` и реальные delivery ZIP. Корневой отчёт обновлён после сборки; проверенные архивы сохраняют документацию своего CI-снимка и при этом ревью не перепаковывались. Исходники и Windows-поставка этим приёмочным аудитом не изменялись.
