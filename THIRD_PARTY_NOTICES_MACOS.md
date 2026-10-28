# LMZ Signal 2M 0.4.0 для macOS — сторонние компоненты

Этот файл в UTF-8 относится к macOS Universal VST3. Он перечисляет использованные локальные оригиналы уведомлений. Полные тексты находятся внутри `Signal2M.vst3/Contents/Resources/Legal/licenses`; уведомления добавляются до подписи, после подписи пакет не изменяется. Запись не назначает лицензию новому коду Signal2M и не подтверждает наличие дополнительных коммерческих соглашений.

## JUCE 7.0.12

Официальный источник: [juce-framework/JUCE](https://github.com/juce-framework/JUCE), commit `4f43011b96eb0636104cb3e433894cda98243626`. Copyright в заголовках модулей: Copyright (c) 2022 — Raw Material Software Limited.

`licenses/JUCE-LICENSE.md` содержит исходные условия JUCE 7. Главные заголовки `juce_core`, `juce_audio_basics`, `juce_audio_devices`, `juce_events` содержат ISC-условия. Другие используемые модули содержат GPL v3 / Commercial условия. В `licenses/JUCE-modules` сохранены главные заголовки всех подключаемых модулей: core, audio_basics, audio_devices, events, graphics, gui_basics, gui_extra, audio_formats, audio_processors, audio_utils, audio_plugin_client и data_structures. Дополнительно сохранён исходный `JUCE-README.md`.

Две локальные правки совместимости перечислены в `tools/juce7-clang23.patch` и проверяются bootstrap: явный `<type_traits>` и ссылка на массив в конструкторе `AudioPluginInstance`. Закреплённая копия с этими правками не выдаётся за неизменённый upstream.

## Steinberg VST3 SDK из закреплённой копии JUCE

Используется SDK, входящий в этот commit JUCE. Сохранены отдельные оригиналы `VST3-SDK-LICENSE.txt`, `VST3-PLUGINTERFACES-LICENSE.txt`, `VST3-BASE-LICENSE.txt`, `VST3-PUBLIC-SDK-LICENSE.txt` и приложенный `VST3-License-Agreement.pdf`.

Корневой и pluginterfaces LICENSE этой версии описывают альтернативу Steinberg VST3 / GPL v3. base и public.sdk содержат собственные условия перераспространения с copyright, ограничениями и disclaimer. Наличие копии соглашения в комплекте не означает, что оно подписано. Отдельные файлы SDK могут сохранять собственные применимые уведомления.

## Встроенные библиотеки

| Компонент | Оригинал в `licenses` |
| --- | --- |
| zlib | `ZLIB-NOTICE.h` |
| libpng | `PNG-LICENSE.txt` |
| IJG JPEG | `JPEG-README.txt` |
| FLAC | `FLAC-LICENSE.txt` |
| Ogg / Vorbis | `OGG-VORBIS-LICENSE.txt`, `LIBVORBIS-COPYING.txt` |

Для IJG сохранена требуемая формулировка: **this software is based in part on the work of the Independent JPEG Group**. Полные сведения об авторах, условиях и отказах от гарантий находятся в перечисленных оригиналах.

## macOS и инструменты сборки

Mac-издание собирается Apple clang и использует системные framework macOS, включая Foundation для HTTPS. Набор динамических зависимостей обоих срезов проверяется через `otool` и сохраняется в `Contents/Resources/Build/MANIFEST.json`. SDK, Xcode, Python, Git, CMake и Ninja не включаются в пользовательский VST3 или ZIP как инструменты.

LLVM-MinGW, UCRT, winpthreads и NSIS относятся к отдельной Windows-поставке. Их бинарники и уведомления в этот Mac-комплект не добавляются. Уведомления CMake/Ninja, используемых только для сборки, остаются в `.tools/macos/python/*dist-info/licenses` исходной рабочей копии.

Векторы логотипа и переключателей встроены в модуль; их исходники находятся в `assets` исходного репозитория. Сведения о происхождении логотипа сохранены внутри пакета в `Contents/Resources/Legal/LOGO_SOURCE.md`. Сведения о цифровой модели и исходных схемах остаются в документации проекта.
