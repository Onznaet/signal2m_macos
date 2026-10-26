"""Independent delivery checks. Reads files/ZIP as UTF-8 explicitly; no writes."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
# Filled only after the current startup-check/name-change module passes the CLI host.
VERIFIED_BINARY_SHA256 = "adb0a3823ff7aa0b1bc8bc24f5a3c5c05c758963b4eae1a9c3f86e5638a84b08"
VERSION = "0.3.1"
HOST_NAME = "LMZ Signal 2M"
ZIP = ROOT / "dist/Signal2M-v0.3.1-Windows-x64.zip"
EXTRACTED = ROOT / "dist/Signal2M-v0.1-Windows-x64"
BUNDLE = ROOT / "build/Signal2M_artefacts/Release/VST3/Signal2M.vst3"
BINARY_NAME = "Signal2M.vst3/Contents/x86_64-win/Signal2M.vst3"
UPDATE_SOURCE = "https://docs.google.com/spreadsheets/d/1ZyDHZskIvieqR_L6y4Ghu0gwvc6usByiNUF52y6AidM/export?format=csv&gid=0&range=A1%3AB1"


def check(condition: bool, description: str) -> None:
    print(("PASS " if condition else "FAIL ") + description)
    if not condition:
        raise AssertionError(description)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    for output in (sys.stdout, sys.stderr):
        if hasattr(output, "reconfigure"):
            output.reconfigure(encoding="utf-8", errors="strict")
    check(len(VERIFIED_BINARY_SHA256) == 64, "independent 0.3.1 binary hash is recorded before package approval")
    check(ZIP.is_file(), "0.3.1 delivery ZIP exists")
    checksum = ZIP.with_suffix(".zip.sha256").read_text(encoding="utf-8").split()[0]
    check(checksum == sha(ZIP.read_bytes()), "external ZIP SHA-256 matches the actual archive")
    with zipfile.ZipFile(ZIP) as archive:
        check(archive.testzip() is None, "all ZIP CRCs pass")
        names = archive.namelist()
        check(len(names) == len(set(names)), "no duplicate ZIP entry names")
        check(all(not PurePosixPath(name).is_absolute() and ".." not in PurePosixPath(name).parts
                  and ":" not in name and "\\" not in name for name in names), "all package entries are safe relative paths")
        for name in (BINARY_NAME, "INSTALL_RU.txt", "VERSION_NOTES_RU.txt", "REVIEW_REPORT.md", "THIRD_PARTY_NOTICES.md", "MANIFEST.json"):
            check(name in names, "required delivery entry: " + name)
        binary = archive.read(BINARY_NAME)
        check(binary[:2] == b"MZ", "module has a PE DOS header")
        pe_offset = struct.unpack_from("<I", binary, 0x3C)[0]
        check(binary[pe_offset:pe_offset + 4] == b"PE\0\0" and struct.unpack_from("<H", binary, pe_offset + 4)[0] == 0x8664,
              "module is Windows PE x86_64")
        check(sha(binary) == VERIFIED_BINARY_SHA256, "archive contains the exact independently loaded/tested module")
        check(HOST_NAME.encode("utf-8") in binary, "new host-visible product name is compiled in the checked module")
        check(b"https://t.me/lenmodzvuk" in binary,
              "the supplied Telegram destination is embedded in the tested module")
        check(UPDATE_SOURCE.encode("utf-8") in binary,
              "the fixed public A1:B1 CSV source is embedded in the tested module")
        check(b"https://vk.ru/lmzvuk" not in binary,
              "the currently observed B1 update page is dynamic, not a compiled fallback")
        for label in ("Проверить обновления", "Проверяем…", "Обновлений нет", "Доступна версия ",
                      "Перейти к обновлениям", "Не удалось проверить обновления. Попробуйте позже", "Новая версия"):
            check(label.encode("utf-8") in binary, "update UI label is present: " + label)
        if int(VERSION.split(".")[0]) == 0:
            check(b"BETA" in binary, "BETA artwork is included for this major-zero build")
        metadata = archive.read("Signal2M.vst3/Contents/Resources/moduleinfo.json").decode("utf-8")
        # The pinned Steinberg helper writes trailing commas; match all version
        # fields without treating its JSON5-style output as strict JSON.
        metadata_versions = re.findall(r'"Version"\s*:\s*"([^"]+)"', metadata)
        check(bool(metadata_versions) and set(metadata_versions) == {VERSION},
              "moduleinfo and every factory class advertise version 0.3.1")
        class_names = re.findall(r'"Category"\s*:\s*"(?:Audio Module Class|Component Controller Class)"\s*,\s*"Name"\s*:\s*"([^"]+)"', metadata)
        check(len(class_names) == 2 and set(class_names) == {HOST_NAME},
              "moduleinfo audio/controller names are LMZ Signal 2M while the bundle filename is preserved")
        for asset_name in ("lmz-logo.svg", "toggle-on.svg", "toggle-off.svg", "LOGO_SOURCE.md"):
            archive_name = "artwork/" + asset_name
            check(archive_name in names and archive.read(archive_name) == (ROOT / "assets" / asset_name).read_bytes(),
                  "delivered artwork matches the reviewed source: " + asset_name)
            archive.read(archive_name).decode("utf-8")
        for asset_name in ("lmz-logo.svg", "toggle-on.svg", "toggle-off.svg"):
            asset = (ROOT / "assets" / asset_name).read_bytes()
            svg_text = asset.decode("utf-8")
            svg = ET.fromstring(svg_text)
            tags = {element.tag.rsplit("}", 1)[-1] for element in svg.iter()}
            check(svg.tag.rsplit("}", 1)[-1] == "svg" and "path" in tags,
                  "valid vector SVG asset: " + asset_name)
            check(not tags.intersection({"text", "image", "foreignObject", "script", "style"}),
                  "asset has no font, raster, script or stylesheet dependency: " + asset_name)
            if asset_name == "lmz-logo.svg":
                check(tags <= {"svg", "g", "defs", "path", "title", "desc", "metadata"},
                      "both waveform and original lettering are path outlines")
            check(all(not key.rsplit("}", 1)[-1].endswith("href") or value.startswith("#")
                      for element in svg.iter() for key, value in element.attrib.items())
                  and all(target.strip(" \t\r\n\"'").startswith("#")
                          for target in re.findall(r"url\(([^)]+)\)", svg_text)),
                  "asset references are self-contained: " + asset_name)
            check(asset in binary, "current SVG asset bytes are embedded in the tested module: " + asset_name)
        bundle_files = {"Signal2M.vst3/" + item.relative_to(BUNDLE).as_posix(): item
                        for item in BUNDLE.rglob("*") if item.is_file()}
        archived_bundle = {name for name in names if name.startswith("Signal2M.vst3/") and not name.endswith("/")}
        check(archived_bundle == set(bundle_files), "complete current SDK-created VST3 bundle is preserved")
        check(all(sha(archive.read(name)) == sha(path.read_bytes()) for name, path in bundle_files.items()),
              "every VST3 bundle file matches the build output")
        check(not any(name.startswith((".tools/", "dependencies/", "build/", "Tests/", "Source/"))
                      or name.lower().endswith((".exe", ".pdb", ".dll")) for name in names), "developer tools, test helpers and runtime DLLs are excluded")
        for name in ("INSTALL_RU.txt", "VERSION_NOTES_RU.txt", "REVIEW_REPORT.md", "THIRD_PARTY_NOTICES.md"):
            contents = archive.read(name).decode("utf-8")
            check("\ufffd" not in contents, "UTF-8 text without replacement characters: " + name)
            check(archive.read(name) == (ROOT / name).read_bytes(), "delivered text matches current reviewed file: " + name)
        install = archive.read("INSTALL_RU.txt").decode("utf-8")
        notes = archive.read("VERSION_NOTES_RU.txt").decode("utf-8")
        check("C:\\Program Files\\Common Files\\VST3" in install and "целиком" in install,
              "installation describes copying the complete bundle to the standard VST3 directory")
        check("Без нот выход молчит" in install and "MIDI note 60" in install and "5 мс" in install, "MIDI silence, neutral key and finite note duration are explained")
        check("сердцевины" in install.lower() and "лампа" in install.lower() and "0,5" in notes, "rotating caps and the authorised LFO LEDs are disclosed")
        check("0.3" in notes and "SVG" in notes and "t.me/lenmodzvuk" in notes and "донат" in notes.lower(),
              "0.3 artwork, support popup and donation placeholders are documented")
        check("https://t.me/lenmodzvuk" in install + notes and "Esc" in install + notes,
              "the user-provided Telegram destination and popup dismissal are explained")
        check("Проверить обновления" in install and "Проверяем…" in install
              and "Перейти к обновлениям" in install and "B1" in install,
              "installation explains explicit checking and the dynamic B1 browser link")
        check("8 секунд" in install and "64 KiB" in notes,
              "bounded background network limits are disclosed")
        check("Новая версия" in install + notes and "перв" in (install + notes).lower()
              and "редактор" in (install + notes).lower(), "first-editor startup checking and the logo badge are disclosed")
        check(VERSION in install and HOST_NAME in install and "BETA" in install and "1.0.0" in install,
              "installed version is 0.3.1 with the new display name and conditional BETA")
        check("фиксац" in notes and "не подтвержден" in notes, "provisional latching and unverified hardware equivalence are disclosed")
        manifest = json.loads(archive.read("MANIFEST.json").decode("utf-8"))
        check(manifest.get("product") == HOST_NAME and manifest.get("version") == VERSION
              and manifest.get("platform") == "Windows x64" and manifest.get("format") == "VST3",
              "manifest product LMZ Signal 2M, version 0.3.1, platform and format")
        check(manifest.get("manual_daw_and_visual_checks") == "pending user", "manual DAW/visual acceptance is honestly pending")
        files = manifest["files"]
        check(set(files) == {name for name in names if name != "MANIFEST.json" and not name.endswith("/")},
              "manifest accounts for every payload file")
        check(all(sha(archive.read(name)) == expected for name, expected in files.items()), "all manifest SHA-256 values match payload")
        runtime = ".tools/llvm-mingw-20261020-ucrt-x86_64/"
        sdk = "dependencies/JUCE/modules/juce_audio_processors/format_types/VST3_SDK/"
        required_notices = [
            "dependencies/JUCE/LICENSE.md", sdk + "LICENSE.txt", sdk + "pluginterfaces/LICENSE.txt",
            sdk + "base/LICENSE.txt", sdk + "public.sdk/LICENSE.txt", sdk + "VST3_License_Agreement.pdf",
            "dependencies/JUCE/modules/juce_graphics/image_formats/pnglib/LICENSE",
            "dependencies/JUCE/modules/juce_graphics/image_formats/jpglib/README",
            "dependencies/JUCE/modules/juce_audio_formats/codecs/flac/Flac Licence.txt",
            "dependencies/JUCE/modules/juce_audio_formats/codecs/oggvorbis/Ogg Vorbis Licence.txt",
            runtime + "LICENSE.TXT",
            runtime + "x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt",
            "installer/NSIS-COPYING.txt",
        ]
        for module in ("juce_audio_basics", "juce_audio_devices", "juce_core", "juce_events", "juce_graphics",
                       "juce_gui_basics", "juce_gui_extra", "juce_audio_formats", "juce_audio_processors",
                       "juce_audio_utils", "juce_audio_plugin_client", "juce_data_structures"):
            required_notices.append(f"dependencies/JUCE/modules/{module}/{module}.h")
        notice_hashes = {sha(archive.read(name)) for name in names if name.startswith("licenses/") and not name.endswith("/")}
        for relative in required_notices:
            check(sha((ROOT / relative).read_bytes()) in notice_hashes, "original notice included unchanged: " + relative)
        check(EXTRACTED.is_dir(), "established legacy-named copy-from directory exists")
        check(all((EXTRACTED / name).is_file() and (EXTRACTED / name).read_bytes() == archive.read(name)
                  for name in names if not name.endswith("/")), "every v0.3.1 ZIP file matches the updated existing copy-from directory")
        extracted_manifest = json.loads((EXTRACTED / "MANIFEST.json").read_text(encoding="utf-8"))
        check(extracted_manifest.get("version") == VERSION, "legacy directory name contains v0.3.1 payload metadata")
        print("RESULT delivery_checks=PASS entries=" + str(len(names)) + " bytes=" + str(ZIP.stat().st_size))
        print("ARCHIVE_SHA256 " + sha(ZIP.read_bytes()))
        print("BINARY_SHA256 " + sha(binary))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, KeyError, OSError, ValueError, ET.ParseError, zipfile.BadZipFile) as error:
        print("ERROR " + str(error), file=sys.stderr)
        raise SystemExit(1)
