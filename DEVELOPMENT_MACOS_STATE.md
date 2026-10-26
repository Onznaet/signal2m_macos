# LMZ Signal 2M — состояние Mac-порта

27 октября 2026, Europe/Moscow. Пользователь изменил маршрут: требуется опубликовать проект в GitHub и действительно собрать Mac VST3 через GitHub Actions. Команда `bash tools/build_macos.sh` для Mac друга остаётся запасным вариантом. В Windows нет локального macOS SDK. Нативный Mac-бинарник пока не собран.

## Текущий доступ к GitHub

CLI `gh auth status` сообщает о недействующем сохранённом входе. Поиск интеграций нашёл GitHub, но `installed=false`; пользователю предложена установка. Секреты не извлекались и не запрашивались в чате. Загрузка исходников и запуск Actions авторизованы текущим запросом пользователя, пользователь явно выбрал публичный репозиторий `Onznaet/signal2m_macos`. Публичный стандартный Actions бесплатен; приватный использует лимит аккаунта, отсутствие расходов без проверки не гарантируется.

Workflow локально подготовлен для стандартных `macos-15-intel` и `macos-15`, включает сохранение диагностик даже после ошибки, артефакты включены по умолчанию и хранятся 7 дней. Публикация будет из отдельной папки с проверенным списком исходников, без `.tools`, build, личных пресетов и credentials. Пользователь создал публичный Onznaet/signal2m_macos с initial README. В открытой Chrome-вкладке выполнен вход; пользователь выбрал ручной выбор/перетаскивание подготовленных файлов. Сборка Actions ещё не запускалась. CLI OAuth прекращён, токен для браузерного маршрута не нужен.

## Зафиксировано

- Исходная версия 0.3.1, host name `LMZ Signal 2M`, bundle `Signal2M.vst3`, все прежние коды, IDs и state сохраняются. Цель — Universal arm64+x86_64, macOS 10.15 Intel / 11 Apple Silicon.
- Mac обновления: отдельный gid `640439888`, Windows gid `0`. A1/B1 и spreadsheet прежние. Только read-only запрос Mac CSV выполнен; получено HTTP200 и `0.3.1,https://vk.ru/lmzvuk`. Ячейки не менялись.
- Добавлен нативный Foundation/NSURLSession transport, изолированный от Windows WinHTTP. Сохранены ограничения запроса, anonymous/TLS и фоновые вызовы. Политика завершения: bounded native cleanup, уникальный runtime delegate class на image, при патологическом незавершении один permanent image pin и circuit breaker. Нативную корректность этих путей нужно подтвердить стресс-тестом.
- Общий updater suite перенесён на POSIX/Mac и запускает прямые HTTP probes вне main thread. Независимый Mac CFBundle/VST3 host, dylib fixture/lifecycle checker, тестовый plist для loopback и проверка fat Mach-O/ZIP подготовлены.
- Mac bootstrap/build/package и ручной CI локально подготовлены; JUCE/CMake/Ninja закреплены. Для первого теста предусмотрена ad-hoc подпись, не Developer ID и не нотарификация. Никаких внешних uploads/workflow runs, установки в систему или запуска DAW не было.
- Миграция прежних папок Windows-пресетов ранее отложена пользователем. Порт не переименовывает плагин повторно и не трогает пользовательские пресеты.

## Фактически выполнено здесь

- Полная текущая Windows-сборка: PASS. Лог `build/mac/windows-regression-build.txt`.
- CTest после платформенных изменений: **8/8 PASS**, 27.91 секунды. Включены реальные Windows VST3/параметры/state/MIDI/DSP/updater и независимый parser Mac-пакета. Лог `build/mac/windows-regression-ctest.txt`. Графика/DAW не запускались.
- Проверки Mac tooling, manifest, UTF-8/LF, зависимостей, ZIP modes/symlink/hash, отрицательные fixtures и shell syntax: результаты `build-macos/tooling-windows-checks.json`.
- Objective-C++ mock syntax checks выполнялись на Windows с декларационными stub headers и clang 23; результаты `build/mac/transport-mock-syntax-results.json`. Они не являются компиляцией с Apple SDK или выполнением Foundation.
- Существующий Windows `dist` и установщик не перепаковывались в ходе Mac-порта.

## Следующий шаг

Передать пользователю `dist-macos/Signal2M-v0.3.1-macOS-SOURCE.zip` после независимой проверки его состава. Это исходный комплект; в нём отсутствует готовый Mach-O/VST3.

Друг распаковывает комплект, проверяет prerequisites из `MACOS_HANDOFF_RU.txt` и запускает `bash tools/build_macos.sh`. После успеха нужны Mac ZIP, SHA-256, `build-macos/native-test-results.json` и LastTest.log. После ошибки нужен полный текст ошибки; не объявлять сборку успешной по наличию одного файла.

Нативная сборка, фактическая подпись, загрузка и обработка Mac VST3, HTTP/TLS и dylib lifetime, обе реальные runtime-архитектуры, Gatekeeper/DAW и самые ранние целевые macOS остаются ожидающими. Одна машина подтверждает только свой нативный срез. Независимый итог — `REVIEW_MACOS_REPORT.md`; пользовательская визуальная приёмка — на Mac друга.
