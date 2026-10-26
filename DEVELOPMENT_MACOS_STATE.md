# LMZ Signal 2M — состояние Mac-порта

27 октября 2026, Europe/Moscow. Нативная сборка macOS Universal 0.3.1 завершена через GitHub Actions. В run5 оба задания — Apple Silicon и Intel — завершились успешно: по **9/9 CTest**, проверка подписи, упаковка и загрузка извлечённого из ZIP VST3 независимым host.

- Репозиторий: [Onznaet/signal2m_macos](https://github.com/Onznaet/signal2m_macos).
- Успешный прогон: [run5 — 40419330616](https://github.com/Onznaet/signal2m_macos/actions/runs/40419330616).
- Исходный commit сборки: c3dc36f91ade3c6e5194ffceccd5a009faeab2ae.
- Версия: 0.3.1; host name LMZ Signal 2M; bundle Signal2M.vst3; прежние IDs, параметры и state сохранены.
- В обоих Universal-пакетах присутствуют arm64 и x86_64. Фактические минимумы load commands проверены: macOS 11.0 arm64 / 10.15 Intel.

## Основная доставка

Готовый пакет: dist-macos/Signal2M-v0.3.1-macOS-Universal.zip, **5 767 813 байт**. Это Universal-сборка, нативно проверенная на arm64.

```text
d916a9fdbc52578769a74ad1e58ee8f5faa7c57dc767548344f4e6ed28e9aedf
```

Подпись — **ad-hoc**, Developer ID отсутствует, нотариализация Apple не выполнялась. Инструкция — MACOS_HANDOFF_RU.txt и docs/INSTALL_MACOS_RU.txt; в DAW устанавливается целая папка Signal2M.vst3.

Intel job проверил тот же commit отдельной Universal-сборкой на x86_64. Её ZIP имеет другой хеш и не заменяет основную доставку:

| Сборка run5 | Нативные тесты и archived host | Размер ZIP | SHA-256 ZIP |
| --- | --- | --- | --- |
| arm64, основная | 9/9 PASS; извлечённый VST3 PASS | 5 767 813 | d916a9fdbc52578769a74ad1e58ee8f5faa7c57dc767548344f4e6ed28e9aedf |
| x86_64, отдельная | 9/9 PASS; извлечённый VST3 PASS | 5 767 812 | 5705d4f62db7cefb7088109a1b0ca99d39e911b2cb511540dcc5a6a18d62e3d0 |

Каждый job запускал свой нативный срез собственной сборки. Эти два архива не объявляются побитово одинаковыми.

## Подтверждено

Нативные проверки охватывают DSP, MIDI/NoteVoice, параметры и state, загрузку реального CFBundle/VST3, updater HTTP/TLS/privacy/cancellation и время жизни отдельного динамического модуля. Пакет прошёл проверки архитектур, plist/moduleinfo, минимальных ОС и системных зависимостей, ad-hoc подписи, ZIP CRC/SHA-256/прав/ссылок и native host после распаковки.

Сохранённые результаты: build/github-publish/run5-arm64 и build/github-publish/run5-x86_64. Актуальный независимый итог — REVIEW_MACOS_REPORT.md в репозитории. Копия отчёта внутри уже доставленного ZIP остаётся историческим снимком исходников на момент сборки и может содержать прежний статус pending. ZIP после проверок не изменялся.

Windows-регрессия ранее прошла **8/8 PASS**, 27.91 с: build/mac/windows-regression-ctest.txt. Windows dist и установщик в ходе Mac-доставки не перепаковывались. Mac читает отдельный update gid 640439888; Windows сохраняет gid=0. Таблица не редактировалась.

## Остаётся ручная приёмка

Нужны установка на Mac пользователя, проверка в DAW, внешний вид, открытие/закрытие редактора, автоматизация и реальные проекты. Запуск на минимальных macOS 10.15 Intel и 11.0 Apple Silicon отдельно не выполнялся; нативные CI jobs работали на macOS 15. Developer ID, нотариализация и проверка поведения Gatekeeper для публичной поставки остаются отдельными шагами.

Для повторной локальной сборки остаётся bash tools/build_macos.sh; готовому VST3 Xcode, Python и инструменты сборки не нужны.
