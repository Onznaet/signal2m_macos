# Сборка LMZ Signal 2M 0.3.1 для macOS

Рецепт создаёт один VST3-пакет `Signal2M.vst3` с архитектурами **arm64 и x86_64**. Имя инструмента в DAW — **LMZ Signal 2M**. Версия первого издания для Mac остаётся `0.3.1`; bundle ID `sound.lmz.signal2m`, manufacturer `Lmzz`, plugin code `Sg2m` и идентификаторы параметров сохранены.

Минимальная цель компиляции — macOS 10.15 для Intel и 11.0 для Apple Silicon. Apple clang поднимает минимум arm64 до 11.0; упаковщик проверяет фактические load commands каждого среза. Это не заменяет запуск на старых версиях macOS: CI ниже проверяет runtime на macOS 15, а работа на минимальных ОС пока требует отдельного прогона.

## Один запуск на Mac

Нужны Mac с Intel или Apple Silicon, установленный **нативный Python 3.9+ с pip**, Git и активные Xcode Command Line Tools с macOS SDK. Полный Xcode также подходит. Если developer tools ещё нет, установите их штатным способом (`xcode-select --install`) и завершите установку до сборки. Сам рецепт не устанавливает Xcode, Homebrew, Rosetta и глобальные пакеты.

Откройте Terminal в корне полученной рабочей копии проекта и выполните:

```bash
bash tools/build_macos.sh
```

Путь проекта может содержать пробелы. Shell-скрипт передаёт аргументы в Python без конкатенации команд. На Apple Silicon используется arm64 Python; запуск Python под Rosetta прекращается с понятным сообщением, чтобы нативная проверка не превратилась в проверку Intel-среза.

Команда последовательно проверяет SDK, готовит зависимости, собирает Universal VST3, выполняет CTest в нативной архитектуре текущего Mac, затем проверяет и упаковывает доставку. Она не копирует плагин в системные или пользовательские папки DAW и не открывает DAW.

Результаты успешного нативного запуска:

```text
build-macos/Signal2M_artefacts/Release/VST3/Signal2M.vst3
build-macos/native-test-results.json
dist-macos/Signal2M-v0.3.1-macOS-Universal.zip
dist-macos/Signal2M-v0.3.1-macOS-Universal.zip.sha256
```

Каталоги `build-macos`, `dist-macos` и `.tools/macos` отделены от Windows `build`, `dist` и `.tools/python`. Существующая Windows-поставка не перезаписывается.

## Закреплённые зависимости

| Компонент | Версия / источник | Локальный путь |
| --- | --- | --- |
| Apple clang и macOS SDK | активные Xcode/Command Line Tools на Mac | показывает `xcrun` |
| JUCE | 7.0.12, commit `4f43011b96eb0636104cb3e433894cda98243626` | `dependencies/JUCE` |
| CMake, wheel PyPI | 3.31.6 | `.tools/macos/python/cmake/data/bin` |
| Ninja, wheel PyPI | 1.11.1.3 | `.tools/macos/python/bin/ninja` |
| Языки | C++17, Objective-C++17 с ARC для сетевого транспорта | `Source`, `Tests` |

Bootstrap использует стандартную библиотеку Python и `pip --isolated --target` только внутри проекта. JUCE клонируется из официального репозитория; реальный commit сверяется с закреплённым значением. Применяются две проверяемые правки из существующего `tools/juce7-clang23.patch`: явный `<type_traits>` и ссылка на массив в `AudioPluginInstance`. Проверяется полный текст обоих файлов. Неожиданные изменения JUCE сохраняются, а сборка останавливается. Локальная/глобальная конфигурация Git не меняется, проверка TLS не отключается. Windows-компилятор LLVM-MinGW и NSIS macOS-рецептом не скачиваются.

Зависимости можно подготовить отдельно или проверить без загрузок и изменений:

```bash
python3 -X utf8 tools/bootstrap_mac.py
python3 -X utf8 tools/bootstrap_mac.py --check
```

Повторная полная сборка без загрузок и патчей, с ограничением параллелизма:

```bash
bash tools/build_macos.sh --skip-bootstrap --jobs 4
```

Для одной сборки и технических тестов без ZIP:

```bash
bash tools/build_macos.sh --no-package
```

## Обновления для macOS

Mac использует отдельный лист Google-таблицы: **gid `640439888`**. Windows продолжает читать свой лист `gid=0`. В Mac-листе A1 содержит числовую версию, B1 — ссылку на страницу обновления для Mac. Значения не меняются скриптами сборки.

```text
https://docs.google.com/spreadsheets/d/1ZyDHZskIvieqR_L6y4Ghu0gwvc6usByiNUF52y6AidM/export?format=csv&gid=640439888&range=A1%3AB1
```

URL внедряется через CMake compile definition `SIGNAL2M_UPDATE_FEED_URL`. Cache-переменные: `SIGNAL2M_UPDATE_SHEET_ID` и `SIGNAL2M_MAC_UPDATE_GID`. Пустой либо нечисловой Mac gid вызывает ошибку конфигурации. Для явной смены Mac-листа:

```bash
bash tools/build_macos.sh --mac-update-gid 640439888
```

Нативный сетевой транспорт macOS использует Foundation; WinHTTP подключается только в Windows. Браузер, curl и web view в плагине не требуются. Звуковая обработка не запускает HTTP-запросов.

## Что проверяется

CTest выполняет переносимые DSP/NoteVoice/Processor тесты, общие проверки update checker, дополнительные тесты Mac-транспорта и независимый VST3 host через реальные `bundleEntry`, `GetPluginFactory`, `bundleExit`. Host получает путь целого bundle; он проверяет фабрику, MIDI, звук, параметры, восстановление состояния и прежний fixture 0.1. Отдельный минимальный dylib и не связанный с JUCE host проверяют отмену запроса и реальную загрузку/выгрузку сетевого транспорта. Принудительный fallback включён только в этом тестовом dylib. Windows DLL-loader в Mac-цели не включается.

Перед CTest финальный bundle после записи `moduleinfo.json` получает локальную ad-hoc подпись. Упаковщик отдельно проверяет `lipo`, минимумы ОС и системные зависимости через `otool`, plist, версию и классы `moduleinfo.json`. Полный bundle копируется в staging через `ditto`, подписывается, проходит `codesign --verify --deep --strict --all-architectures` и только затем помещается в ZIP через `ditto`. После распаковки проверяются подпись, CRC, SHA-256, права и символические ссылки; независимый VST3 host загружает именно извлечённый из ZIP пакет. Manifest отдельно фиксирует SHA-256 протестированного исходного модуля и подписанного доставляемого модуля. В нём сохраняется архитектура фактически выполненных тестов; один запуск на Intel не объявляется проверкой arm64 и наоборот.

Пакет включает инструкции, отдельные Mac-уведомления, оригиналы лицензий JUCE/VST3 и встроенных библиотек, исходные SVG. LLVM-MinGW и NSIS в Mac-комплект не добавляются.

## Подпись и передача

По умолчанию используется **локальная ad-hoc подпись**. Она подходит для технической проверки локально собранного плагина и не является Developer ID или нотариализацией Apple. Manifest явно содержит `notarization: not-performed`. Полученный ZIP не объявляется готовым публичным нотариализованным релизом.

Если владелец уже настроил Developer ID Application в Keychain и намеренно хочет применить эту локальную подпись, можно явно передать её имя:

```bash
bash tools/build_macos.sh --sign-identity 'Developer ID Application: YOUR NAME (TEAMID)'
```

Упаковщик проверяет фактический тип подписи. Ключи, сертификаты, пароли и notary-профили в проект не добавляются. Обращения к сервису нотариализации, загрузки Apple и установка сертификатов этим рецептом не выполняются.

Готовый bundle устанавливается вручную по `docs/INSTALL_MACOS_RU.txt`. Внешний вид, открытие/закрытие окна, автоматизация и воспроизведение старого проекта проверяются в настоящей DAW после технических тестов.

## GitHub Actions

.github/workflows/macos.yml — ручной workflow с двумя отдельными runner: macos-15-intel (x86_64) и macos-15 (arm64). Оба собирают Universal и запускают соответствующий нативный срез тестов. Имена runner сверены с [официальной таблицей GitHub](https://docs.github.com/en/actions/reference/runners/github-hosted-runners). [Run5 — 40419330616](https://github.com/Onznaet/signal2m_macos/actions/runs/40419330616) завершился успешно на обоих runner; каждый прошёл 9/9 CTest и полный этап упаковки. Сохранение artifacts включено по умолчанию через input upload_artifacts; срок хранения — 7 дней. Каждый job тестирует собственную Universal-сборку, поэтому хеши двух архивов различаются.

Основные источники рецепта: [CMake 3.31: архитектуры](https://cmake.org/cmake/help/v3.31/variable/CMAKE_OSX_ARCHITECTURES.html), [CMake 3.31: deployment target](https://cmake.org/cmake/help/v3.31/variable/CMAKE_OSX_DEPLOYMENT_TARGET.html), [Apple: Code Signing In Depth](https://developer.apple.com/library/archive/technotes/tn2206/_index.html) и закреплённые локальные CMake-файлы JUCE. Параметры Apple toolchain задаются до `project()` согласно документации CMake.

Нативная сборка, тесты и упаковка подтверждены в [run5](https://github.com/Onznaet/signal2m_macos/actions/runs/40419330616) из commit c3dc36f91ade3c6e5194ffceccd5a009faeab2ae: arm64 и x86_64 прошли по **9/9 CTest**, ad-hoc подпись, проверку ZIP и загрузку извлечённого VST3 независимым native host. Это реальные прогоны на macOS 15; локальная Windows-среда использовалась для подготовки и Windows-регрессии.

Основная доставка — dist-macos/Signal2M-v0.3.1-macOS-Universal.zip, **5 767 813 байт**, SHA-256 d916a9fdbc52578769a74ad1e58ee8f5faa7c57dc767548344f4e6ed28e9aedf. Этот пакет содержит обе архитектуры и нативно проверен на arm64. Отдельная Universal-сборка того же commit прошла нативную проверку Intel. Developer ID и нотариализация отсутствуют; DAW/GUI и запуск на минимальных macOS 10.15 Intel / 11.0 Apple Silicon остаются ручными проверками.

Отчёт внутри доставленного ZIP сохранён как исторический снимок исходников на момент сборки и может содержать прежние pending-статусы. Актуальный независимый отчёт — REVIEW_MACOS_REPORT.md в репозитории. Проверенный ZIP не перепаковывался ради обновления документации.
