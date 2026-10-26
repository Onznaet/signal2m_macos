cmake_minimum_required(VERSION 3.22)

# Initialise Apple's toolchain before project() so every target, including JUCE
# helpers, uses the same two slices. A separate build-macos directory keeps the
# existing Windows build cache and delivery untouched.
if(CMAKE_HOST_APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS Universal architectures")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version (arm64 minimum is 11.0)")
endif()
project(Signal2M VERSION 0.3.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(SIGNAL2M_BUILD_TESTS "Build headless technical checks" ON)
set(SIGNAL2M_UPDATE_SHEET_ID "1ZyDHZskIvieqR_L6y4Ghu0gwvc6usByiNUF52y6AidM" CACHE STRING "Google spreadsheet containing the platform update tabs")
set(SIGNAL2M_MAC_UPDATE_GID "640439888" CACHE STRING "Separate macOS update tab gid")
if(NOT SIGNAL2M_UPDATE_SHEET_ID MATCHES "^[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "SIGNAL2M_UPDATE_SHEET_ID must be a Google spreadsheet ID, not a URL")
endif()
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
    if(NOT SIGNAL2M_MAC_UPDATE_GID MATCHES "^[0-9]+$")
        message(FATAL_ERROR "SIGNAL2M_MAC_UPDATE_GID must identify the separate macOS update tab (numeric gid)")
    endif()
    set(SIGNAL2M_UPDATE_GID "${SIGNAL2M_MAC_UPDATE_GID}")
elseif(WIN32)
    set(SIGNAL2M_UPDATE_GID "0")
else()
    message(FATAL_ERROR "Signal2M currently supports Windows and macOS VST3 builds")
endif()
set(SIGNAL2M_CONFIGURED_UPDATE_FEED_URL "https://docs.google.com/spreadsheets/d/${SIGNAL2M_UPDATE_SHEET_ID}/export?format=csv&gid=${SIGNAL2M_UPDATE_GID}&range=A1%3AB1")
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
if(MINGW)
    add_link_options(-static)
endif()
add_subdirectory(dependencies/JUCE EXCLUDE_FROM_ALL)

add_library(Signal2MDsp STATIC Source/SynthEngine.cpp Source/NoteVoice.cpp)
target_include_directories(Signal2MDsp PUBLIC Source)

juce_add_binary_data(Signal2MUiAssets
    HEADER_NAME UiAssets.h
    NAMESPACE BinaryData
    SOURCES assets/lmz-logo.svg assets/toggle-on.svg assets/toggle-off.svg)

juce_add_plugin(Signal2M
    COMPANY_NAME "LMZ"
    COMPANY_WEBSITE ""
    BUNDLE_ID "sound.lmz.signal2m"
    PLUGIN_MANUFACTURER_CODE Lmzz
    PLUGIN_CODE Sg2m
    PRODUCT_NAME "Signal2M"
    PLUGIN_NAME "LMZ Signal 2M"
    DESCRIPTION "Signal 2M MIDI-triggered noise synthesizer"
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Synth)
target_sources(Signal2M PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/UpdateChecker.cpp)
if(WIN32)
    target_sources(Signal2M PRIVATE Source/UpdateHttpTransport.cpp)
    target_link_libraries(Signal2M PRIVATE winhttp)
elseif(APPLE)
    target_sources(Signal2M PRIVATE Source/UpdateHttpTransportMac.mm)
    set_source_files_properties(Source/UpdateHttpTransportMac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    find_library(SIGNAL2M_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    target_link_libraries(Signal2M PRIVATE "${SIGNAL2M_FOUNDATION_FRAMEWORK}")
endif()
target_include_directories(Signal2M PUBLIC Source)
target_compile_definitions(Signal2M PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    SIGNAL2M_UPDATE_FEED_URL="${SIGNAL2M_CONFIGURED_UPDATE_FEED_URL}")
target_link_libraries(Signal2M PRIVATE Signal2MDsp Signal2MUiAssets juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags)

if(APPLE)
    # JUCE's manifest helper writes Resources/moduleinfo.json after its initial
    # signing step. Seal the completed bundle before any native host tests run.
    add_custom_command(TARGET Signal2M_VST3 POST_BUILD
        COMMAND /usr/bin/codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:Signal2M_VST3>"
        COMMAND /usr/bin/codesign --verify --deep --strict --all-architectures --verbose=2 "$<TARGET_BUNDLE_DIR:Signal2M_VST3>"
        VERBATIM)
endif()

if(SIGNAL2M_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ReviewerMacPackageChecks.py")
        add_test(NAME Signal2MMacPackageParser
            COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ReviewerMacPackageChecks.py" --self-test)
    endif()
    add_executable(Signal2MDspTests Tests/DspTests.cpp)
    target_link_libraries(Signal2MDspTests PRIVATE Signal2MDsp)
    add_test(NAME Signal2MDspBehavior COMMAND Signal2MDspTests)

    add_executable(Signal2MNoteVoiceTests Tests/NoteVoiceTests.cpp)
    target_link_libraries(Signal2MNoteVoiceTests PRIVATE Signal2MDsp)
    add_test(NAME Signal2MMidiNoteVoice COMMAND Signal2MNoteVoiceTests)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ReviewerDspChecks.cpp")
        add_executable(Signal2MReviewerChecks Tests/ReviewerDspChecks.cpp)
        target_link_libraries(Signal2MReviewerChecks PRIVATE Signal2MDsp)
        add_test(NAME Signal2MIndependentDsp COMMAND Signal2MReviewerChecks)
    endif()

    add_executable(Signal2MProcessorTests Tests/ProcessorTests.cpp)
    target_include_directories(Signal2MProcessorTests PRIVATE dependencies/JUCE/modules)
    target_compile_definitions(Signal2MProcessorTests PRIVATE
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 SIGNAL2M_EXPECTED_VERSION="${PROJECT_VERSION}")
    target_link_libraries(Signal2MProcessorTests PRIVATE Signal2M)
    add_test(NAME Signal2MParametersAndState COMMAND Signal2MProcessorTests)

    add_executable(Signal2MUpdateCheckerTests Tests/UpdateCheckerTests.cpp)
    target_include_directories(Signal2MUpdateCheckerTests PRIVATE dependencies/JUCE/modules)
    target_compile_definitions(Signal2MUpdateCheckerTests PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
    target_link_libraries(Signal2MUpdateCheckerTests PRIVATE Signal2M)
    if(APPLE)
        # Only these headless fixtures use HTTP loopback. Never add ATS
        # exceptions to the production plugin or to its containing DAW.
        target_link_options(Signal2MUpdateCheckerTests PRIVATE
            "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-loopback-test-Info.plist")
    endif()
    add_test(NAME Signal2MManualUpdateChecker COMMAND Signal2MUpdateCheckerTests)
    set_tests_properties(Signal2MManualUpdateChecker PROPERTIES TIMEOUT 45)

    # The independent DLL/unload suite deliberately uses WinDLL APIs.
    if(WIN32)
    add_library(Signal2MReviewerUpdateFixture SHARED Tests/ReviewerUpdateDll.cpp)
    target_include_directories(Signal2MReviewerUpdateFixture PRIVATE dependencies/JUCE/modules)
    target_compile_definitions(Signal2MReviewerUpdateFixture PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
    target_link_libraries(Signal2MReviewerUpdateFixture PRIVATE Signal2M)
    add_executable(Signal2MReviewerUpdateChecks Tests/ReviewerUpdateChecks.cpp)
    target_include_directories(Signal2MReviewerUpdateChecks PRIVATE dependencies/JUCE/modules)
    target_compile_definitions(Signal2MReviewerUpdateChecks PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
    target_link_libraries(Signal2MReviewerUpdateChecks PRIVATE Signal2M)
    add_dependencies(Signal2MReviewerUpdateChecks Signal2MReviewerUpdateFixture)
    add_test(NAME Signal2MIndependentUpdates
        COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ReviewerUpdateHttpServer.py"
        "$<TARGET_FILE:Signal2MReviewerUpdateChecks>" --dll "$<TARGET_FILE:Signal2MReviewerUpdateFixture>"
        --output "${CMAKE_CURRENT_BINARY_DIR}/reviewer-update-ctest-results.txt")
    set_tests_properties(Signal2MIndependentUpdates PROPERTIES TIMEOUT 90)
    endif()

    set(SIGNAL2M_VST3_SDK "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/JUCE/modules/juce_audio_processors/format_types/VST3_SDK")
    set(SIGNAL2M_VST3_HOST_SOURCES
        ${SIGNAL2M_VST3_SDK}/pluginterfaces/base/funknown.cpp
        ${SIGNAL2M_VST3_SDK}/pluginterfaces/base/coreiids.cpp
        ${SIGNAL2M_VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp
        ${SIGNAL2M_VST3_SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
        ${SIGNAL2M_VST3_SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
        ${SIGNAL2M_VST3_SDK}/public.sdk/source/vst/utility/stringconvert.cpp)
    if(WIN32)
    add_executable(Signal2MVst3Checks Tests/ReviewerVst3Checks.cpp ${SIGNAL2M_VST3_HOST_SOURCES})
    target_include_directories(Signal2MVst3Checks SYSTEM PRIVATE ${SIGNAL2M_VST3_SDK})
    target_link_libraries(Signal2MVst3Checks PRIVATE ole32)
    add_dependencies(Signal2MVst3Checks Signal2M_VST3)
    add_test(NAME Signal2MRealVst3Module COMMAND Signal2MVst3Checks "$<TARGET_FILE:Signal2M_VST3>" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/Signal2M-v0.1-state.bin")
    elseif(APPLE)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ReviewerMacVst3Checks.cpp")
            find_library(SIGNAL2M_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
            add_executable(Signal2MMacVst3Checks Tests/ReviewerMacVst3Checks.cpp ${SIGNAL2M_VST3_HOST_SOURCES})
            target_include_directories(Signal2MMacVst3Checks SYSTEM PRIVATE ${SIGNAL2M_VST3_SDK})
            target_link_libraries(Signal2MMacVst3Checks PRIVATE "${SIGNAL2M_COREFOUNDATION_FRAMEWORK}")
            add_dependencies(Signal2MMacVst3Checks Signal2M_VST3)
            add_test(NAME Signal2MRealMacVst3Module COMMAND Signal2MMacVst3Checks "$<TARGET_BUNDLE_DIR:Signal2M_VST3>" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/Signal2M-v0.1-state.bin")
            set_tests_properties(Signal2MRealMacVst3Module PROPERTIES TIMEOUT 180)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/MacUpdateTransportTests.mm")
            add_executable(Signal2MMacUpdateTransportTests Tests/MacUpdateTransportTests.mm)
            target_include_directories(Signal2MMacUpdateTransportTests PRIVATE dependencies/JUCE/modules)
            target_compile_definitions(Signal2MMacUpdateTransportTests PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
            target_compile_options(Signal2MMacUpdateTransportTests PRIVATE -fobjc-arc)
            target_link_libraries(Signal2MMacUpdateTransportTests PRIVATE Signal2M "${SIGNAL2M_FOUNDATION_FRAMEWORK}")
            target_link_options(Signal2MMacUpdateTransportTests PRIVATE
                "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-loopback-test-Info.plist")
            add_test(NAME Signal2MMacUpdatePolicy COMMAND Signal2MMacUpdateTransportTests)
            set_tests_properties(Signal2MMacUpdatePolicy PROPERTIES TIMEOUT 60)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ReviewerMacUpdateFixture.cpp" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ReviewerMacUpdateLifecycle.cpp")
            # This independent dlopen/dlclose probe deliberately excludes the
            # plugin/UI from its host, so unloading the transport is real.
            add_library(Signal2MReviewerMacUpdateFixture SHARED
                Tests/ReviewerMacUpdateFixture.cpp Source/UpdateChecker.cpp Source/UpdateHttpTransportMac.mm)
            target_include_directories(Signal2MReviewerMacUpdateFixture PRIVATE Source dependencies/JUCE/modules)
            target_compile_definitions(Signal2MReviewerMacUpdateFixture PRIVATE
                JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
                SIGNAL2M_MAC_TRANSPORT_TEST_HOOKS=1
                SIGNAL2M_UPDATE_FEED_URL="${SIGNAL2M_CONFIGURED_UPDATE_FEED_URL}")
            target_link_libraries(Signal2MReviewerMacUpdateFixture PRIVATE
                juce::juce_core juce::juce_recommended_config_flags "${SIGNAL2M_FOUNDATION_FRAMEWORK}")
            add_custom_command(TARGET Signal2MReviewerMacUpdateFixture POST_BUILD
                COMMAND /usr/bin/codesign --force --sign - --timestamp=none "$<TARGET_FILE:Signal2MReviewerMacUpdateFixture>"
                COMMAND /usr/bin/codesign --verify --strict --all-architectures "$<TARGET_FILE:Signal2MReviewerMacUpdateFixture>"
                VERBATIM)
            add_executable(Signal2MReviewerMacUpdateLifecycle Tests/ReviewerMacUpdateLifecycle.cpp)
            target_link_options(Signal2MReviewerMacUpdateLifecycle PRIVATE
                "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-loopback-test-Info.plist")
            add_dependencies(Signal2MReviewerMacUpdateLifecycle Signal2MReviewerMacUpdateFixture)
            add_test(NAME Signal2MRealMacUpdateLifecycle COMMAND Signal2MReviewerMacUpdateLifecycle
                --fixture "$<TARGET_FILE:Signal2MReviewerMacUpdateFixture>")
            set_tests_properties(Signal2MRealMacUpdateLifecycle PROPERTIES TIMEOUT 90)
        endif()
    endif()
endif()
