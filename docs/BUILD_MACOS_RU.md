# Сборка LMZ Signal 2M 0.4.0 RELEASE для macOS

Версия берётся из project(Signal2M VERSION ...) в CMakeLists.txt. Рецепт создаёт Universal VST3 с arm64 и x86_64, сохраняя имя LMZ Signal 2M, bundle Signal2M.vst3, ID sound.lmz.signal2m, manufacturer Lmzz, plugin code Sg2m и прежние class IDs. Версия 0.4.0 содержит 14 параметров, Post, tempo-кнопки T и донатные ссылки текущего Windows-издания.

Mac-рецепт явно задаёт SIGNAL2M_SHOW_BETA=OFF и проверяет BOOL=OFF в CMakeCache. Надпись BETA отключена независимо от major version. Это настройка оформления релиза; тип подписи и нотариализация описаны ниже.

## Один запуск на Mac

Нужны Intel или Apple Silicon Mac, нативный Python 3.9+ с pip, Git и Xcode Command Line Tools с macOS SDK. Из корня проекта:

```bash
bash tools/build_macos.sh
```

Скрипт не устанавливает плагин и не запускает DAW. Зависимости находятся внутри проекта: .tools/macos и dependencies/JUCE. JUCE закреплён на 7.0.12 commit 4f43011b96eb0636104cb3e433894cda98243626 с двумя проверяемыми compatibility patches, CMake — 3.31.6, Ninja wheel — 1.11.1.3. Xcode, Homebrew, Rosetta и глобальные Python-пакеты автоматически не устанавливаются.

Apple toolchain получает deployment target до project(); этот же target явно передаётся вложенному configure juceaide. Целевые минимумы остаются macOS 10.15 Intel / 11.0 arm64 и проверяются по каждому настоящему Mach-O срезу.

```bash
python3 -X utf8 tools/bootstrap_mac.py --check
bash tools/build_macos.sh --skip-bootstrap --jobs 4
bash tools/build_macos.sh --no-package
```

## Итоговый ZIP

Основная доставка 0.4.0: dist-macos/Signal2M-v0.4.0-macOS-Universal.zip. В архиве единственный верхний объект:

```text
Signal2M.vst3/
  Contents/
    Info.plist
    MacOS/Signal2M
    Resources/
      moduleinfo.json
      Legal/
        THIRD_PARTY_NOTICES_MACOS.md
        LOGO_SOURCE.md
        licenses/...
      Build/
        MANIFEST.json
        native-test-results.json
        provenance.json
    _CodeSignature/...
```

Рядом с bundle в пользовательском ZIP нет инструкций, manifest или artwork. Эта инструкция и docs/INSTALL_MACOS_RU.txt остаются в проекте. Все обязательные оригиналы лицензий и notices сохранены внутри Resources/Legal.

Resources/Build добавляется до подписи. Внутренний manifest содержит inventory ресурсов, версию, provenance и фактический native test record. Из его хешей исключены сам manifest, исполняемый Mach-O и _CodeSignature: это предотвращает циклическую зависимость хешей при подписи. Точные итоговые хеши всех подписанных файлов, самого ZIP и фактический результат archived host сохраняются в build-macos/package-results.json вне пользовательского ZIP. Внутренний manifest не использует exclusions как исключения из codesign: подпись охватывает весь bundle по правилам Apple.

## Проверки перед выдачей

Рецепт требует 10 CTest suites: DSP, NoteVoice, независимый DSP, параметры/state, update checker, DonationLinks, настоящий VST3 host, Mac HTTP policy, независимый parser и dynamic-image lifecycle. Каждый Mac запускает свой нативный срез. Оба GitHub jobs собирают Universal; проверки Intel и arm64 относятся к отдельным сборкам одного source commit.

Упаковщик проверяет plist/moduleinfo/IDs/version, lipo, минимальные ОС и системные зависимости otool. Затем копирует bundle в staging, добавляет Legal и Build, подписывает и выполняет codesign --verify --deep --strict --all-architectures. Ditto создаёт ZIP только с Signal2M.vst3, без __MACOSX. После распаковки проверяются полный набор файлов, хеши, права и ссылки, подпись, загрузка извлечённого bundle настоящим native host и независимый package checker. После подписи внутрь bundle ничего не дописывается.

## Обновления и донаты

Mac updates: отдельный лист gid=640439888, диапазон A1:B1. Windows сохраняет gid=0. Общие донатные ссылки: gid=1525027926, A1/A2. Таблица 1ZyDHZskIvieqR_L6y4Ghu0gwvc6usByiNUF52y6AidM задаётся через CMake; сборка не меняет ячейки. Сетевые операции выполняются в фоне; аудио их не запускает.

## Подпись и ручная приёмка

По умолчанию применяется ad-hoc подпись. Developer ID и нотариализация отсутствуют; слово RELEASE и отсутствие BETA не означают нотариализацию. Для уже установленной Developer ID Application identity предусмотрен явный параметр:

```bash
bash tools/build_macos.sh --sign-identity 'Developer ID Application: YOUR NAME (TEAMID)'
```

Секреты не входят в проект. Отправка Apple на нотариализацию этим рецептом не выполняется. Инструкция установки — docs/INSTALL_MACOS_RU.txt. DAW, внешний вид редактора, реальные проекты и запуск на минимальных версиях ОС требуют ручной проверки.

## GitHub Actions и текущий статус

.github/workflows/macos.yml — ручной workflow со стандартными macos-15 и macos-15-intel. Имя delivery вычисляется из версии CMake. Artifact может содержать внешний checksum, package-results.json, native-test-results и логи; пользовательский ZIP внутри него содержит только bundle. Артефакты сохраняются при upload_artifacts=true на 7 дней.

Успешный [run5 версии 0.3.1](https://github.com/Onznaet/signal2m_macos/actions/runs/40419330616) подтверждает прежний Mac-рецепт. Для нового RELEASE 0.4.0 необходим новый нативный прогон; локальные Python/fixture проверки не объявляются успешной Apple SDK сборкой. Готовые Windows 0.4.0 и Mac 0.3.1 не перепаковываются этим изменением.

Первичные источники: [CMake deployment target](https://cmake.org/cmake/help/v3.31/variable/CMAKE_OSX_DEPLOYMENT_TARGET.html), [Apple Code Signing In Depth](https://developer.apple.com/library/archive/technotes/tn2206/_index.html), закреплённые локальные исходники JUCE.
