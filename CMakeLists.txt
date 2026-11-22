cmake_minimum_required(VERSION 3.24)
project(snipline VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
include(CTest)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2 DBus Svg)
find_package(LayerShellQt REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Wayland REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(GStreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.20 gstreamer-app-1.0>=1.20)
pkg_check_modules(PulseAudio REQUIRED IMPORTED_TARGET libpulse)
pkg_check_modules(ZXing REQUIRED IMPORTED_TARGET zxing>=2.0)
pkg_check_modules(WaylandProtocols REQUIRED wayland-protocols>=1.41)
pkg_check_modules(HyprlandProtocols REQUIRED hyprland-protocols>=0.8)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(HYPRLAND_PROTOCOLS_DIR hyprland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/protocols")
function(capture_protocol name xml)
    set(base "${CMAKE_CURRENT_BINARY_DIR}/protocols/${name}")
    add_custom_command(OUTPUT "${base}.h" "${base}.c" "${base}-server.h"
        COMMAND ${WAYLAND_SCANNER} client-header "${xml}" "${base}.h"
        COMMAND ${WAYLAND_SCANNER} server-header "${xml}" "${base}-server.h"
        COMMAND ${WAYLAND_SCANNER} private-code "${xml}" "${base}.c"
        DEPENDS "${xml}" VERBATIM)
    target_sources(snipline_protocols PRIVATE "${base}.c" "${base}.h")
endfunction()
add_library(snipline_protocols STATIC)
target_include_directories(snipline_protocols PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/protocols")
target_link_libraries(snipline_protocols PUBLIC PkgConfig::Wayland)
capture_protocol(ext-toplevel "${WAYLAND_PROTOCOLS_DIR}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml")
capture_protocol(wlr-toplevel "${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml")
capture_protocol(hypr-export "${HYPRLAND_PROTOCOLS_DIR}/protocols/hyprland-toplevel-export-v1.xml")
capture_protocol(hypr-mapping "${HYPRLAND_PROTOCOLS_DIR}/protocols/hyprland-toplevel-mapping-v1.xml")

add_library(snipline_core STATIC
    src/core/geometry.cpp src/core/selection.cpp
    src/core/theme.cpp src/core/exporter.cpp src/core/outputs.cpp src/core/windowtargets.cpp src/core/config.cpp
    src/core/recordingsession.cpp src/core/editorcommand.cpp)
target_include_directories(snipline_core PUBLIC src)
target_link_libraries(snipline_core PUBLIC Qt6::Core Qt6::Gui)
target_compile_options(snipline_core PRIVATE -Wall -Wextra -Wpedantic)

qt_add_executable(snipline_encoder_probe src/encoderprobe_main.cpp)
set_target_properties(snipline_encoder_probe PROPERTIES OUTPUT_NAME snipline-encoder-probe)
target_link_libraries(snipline_encoder_probe PRIVATE Qt6::Core PkgConfig::GStreamer)
target_compile_options(snipline_encoder_probe PRIVATE -Wall -Wextra -Wpedantic)
qt_add_executable(snipline_recognition_worker src/recognitionworker_main.cpp)
set_target_properties(snipline_recognition_worker PROPERTIES OUTPUT_NAME snipline-recognize)
target_compile_definitions(snipline_recognition_worker PRIVATE ZX_USE_UTF8)
target_link_libraries(snipline_recognition_worker PRIVATE Qt6::Gui PkgConfig::ZXing)
target_compile_options(snipline_recognition_worker PRIVATE -Wall -Wextra -Wpedantic)
add_library(snipline_app STATIC src/capture.cpp src/controller.cpp src/windows.cpp src/surfacebarrier.cpp src/windowpicker.cpp src/notification.cpp src/recorder.cpp src/audiosources.cpp src/pulsetap.cpp src/recognition.cpp)
add_dependencies(snipline_app snipline_encoder_probe snipline_recognition_worker)
target_link_libraries(snipline_app PUBLIC snipline_core Qt6::Quick
    Qt6::QuickControls2 Qt6::DBus LayerShellQt::Interface snipline_protocols PkgConfig::GStreamer PkgConfig::PulseAudio)
target_compile_options(snipline_app PRIVATE -Wall -Wextra -Wpedantic)
qt_add_executable(snipline src/main.cpp src/doctor.cpp)
set(SNIPLINE_QML
    qml/Overlay.qml
    qml/Glyph.qml
    qml/IconButton.qml
    qml/AudioSelector.qml
    qml/LanguageSelector.qml
)
# QML is loaded with qrc:/qml/...; keep the SVG files under the same prefix.
set(SNIPLINE_ICONS
    qml/icons/audio-dark.svg
    qml/icons/audio-light.svg
    qml/icons/capture-dark.svg
    qml/icons/capture-light.svg
    qml/icons/check-dark.svg
    qml/icons/check-light.svg
    qml/icons/close-dark.svg
    qml/icons/close-light.svg
    qml/icons/chevron-dark.svg
    qml/icons/chevron-light.svg
    qml/icons/grip-dark.svg
    qml/icons/grip-light.svg
    qml/icons/image-dark.svg
    qml/icons/image-light.svg
    qml/icons/mic-dark.svg
    qml/icons/mic-light.svg
    qml/icons/monitor-dark.svg
    qml/icons/monitor-light.svg
    qml/icons/more-dark.svg
    qml/icons/more-light.svg
    qml/icons/ocr-dark.svg
    qml/icons/ocr-light.svg
    qml/icons/qr-dark.svg
    qml/icons/qr-light.svg
    qml/icons/record-dark.svg
    qml/icons/record-light.svg
    qml/icons/refresh-dark.svg
    qml/icons/refresh-light.svg
    qml/icons/region-dark.svg
    qml/icons/region-light.svg
    qml/icons/settings-dark.svg
    qml/icons/settings-light.svg
    qml/icons/window-dark.svg
    qml/icons/window-light.svg
)
set(SNIPLINE_UI_RESOURCES ${SNIPLINE_QML} ${SNIPLINE_ICONS})
list(APPEND SNIPLINE_UI_RESOURCES
    qml/icons/pause-light.svg qml/icons/pause-dark.svg
    qml/icons/play-light.svg qml/icons/play-dark.svg
    qml/icons/stop-light.svg qml/icons/stop-dark.svg
    qml/icons/discard-light.svg qml/icons/discard-dark.svg
    qml/icons/lens-light.svg qml/icons/lens-dark.svg
    qml/icons/upload-light.svg qml/icons/upload-dark.svg)
qt_add_resources(snipline ui PREFIX "/" FILES ${SNIPLINE_UI_RESOURCES})
target_link_libraries(snipline PRIVATE snipline_app PkgConfig::Wayland Qt6::Svg)
target_compile_options(snipline PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    qt_add_executable(snipline_audio_probe tests/audio_probe.cpp)
    target_link_libraries(snipline_audio_probe PRIVATE snipline_app)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(snipline_editor_tests tests/editor_test.cpp)
    target_link_libraries(snipline_editor_tests PRIVATE snipline_core Qt6::Test)
    add_test(NAME editors COMMAND snipline_editor_tests)
    qt_add_executable(snipline_recognition_tests tests/recognition_test.cpp)
    target_link_libraries(snipline_recognition_tests PRIVATE snipline_app Qt6::Test PkgConfig::ZXing)
    add_test(NAME recognition COMMAND snipline_recognition_tests)
    set_tests_properties(recognition PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(snipline_tests tests/core_test.cpp)
    target_link_libraries(snipline_tests PRIVATE snipline_core Qt6::Test)
    add_test(NAME core COMMAND snipline_tests)
    qt_add_executable(snipline_settings_tests tests/settings_test.cpp)
    target_link_libraries(snipline_settings_tests PRIVATE snipline_app Qt6::Test)
    add_test(NAME settings COMMAND snipline_settings_tests)
    qt_add_executable(snipline_recording_tests tests/recording_test.cpp)
    target_link_libraries(snipline_recording_tests PRIVATE snipline_app Qt6::Test)
    add_test(NAME recording COMMAND snipline_recording_tests)
    qt_add_executable(snipline_audio_tests tests/audio_test.cpp)
    target_link_libraries(snipline_audio_tests PRIVATE snipline_app Qt6::Test)
    add_test(NAME audio COMMAND snipline_audio_tests)
    qt_add_executable(snipline_pulse_tests tests/pulse_test.cpp)
    target_link_libraries(snipline_pulse_tests PRIVATE snipline_app Qt6::Test)
    add_test(NAME isolated_audio COMMAND snipline_pulse_tests)
    qt_add_executable(snipline_target_tests tests/windowtargets_test.cpp)
    target_link_libraries(snipline_target_tests PRIVATE snipline_core Qt6::Test)
    add_test(NAME window_targets COMMAND snipline_target_tests)
    set_tests_properties(core PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(snipline_ui_tests tests/ui_test.cpp)
    qt_add_resources(snipline_ui_tests test_ui PREFIX "/" FILES ${SNIPLINE_UI_RESOURCES})
    target_link_libraries(snipline_ui_tests PRIVATE snipline_app Qt6::Test Qt6::Svg PkgConfig::ZXing)
    add_test(NAME ui COMMAND snipline_ui_tests)
    set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent")
    qt_add_executable(snipline_capture_tests tests/capture_test.cpp)
    target_link_libraries(snipline_capture_tests PRIVATE snipline_app Qt6::Test)
    add_test(NAME capture COMMAND snipline_capture_tests)
    pkg_check_modules(WaylandServer REQUIRED IMPORTED_TARGET wayland-server)
    qt_add_executable(snipline_window_tests tests/window_test.cpp)
    target_link_libraries(snipline_window_tests PRIVATE snipline_app Qt6::Test PkgConfig::WaylandServer)
    add_test(NAME windows COMMAND snipline_window_tests)
    find_program(DBUS_RUN_SESSION dbus-run-session)
    if(DBUS_RUN_SESSION)
        qt_add_executable(snipline_notification_tests tests/notification_test.cpp)
        target_link_libraries(snipline_notification_tests PRIVATE snipline_app Qt6::Test)
        add_test(NAME notifications COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:snipline_notification_tests>)
        qt_add_executable(snipline_instance_tests tests/instance_test.cpp)
        target_link_libraries(snipline_instance_tests PRIVATE Qt6::Test Qt6::DBus)
        add_test(NAME instance COMMAND ${DBUS_RUN_SESSION} --
            ${CMAKE_COMMAND} -E env "SNIPLINE_BINARY=$<TARGET_FILE:snipline>"
            "QT_QPA_PLATFORM=offscreen" $<TARGET_FILE:snipline_instance_tests>)
    endif()
endif()

install(TARGETS snipline snipline_encoder_probe snipline_recognition_worker RUNTIME DESTINATION bin)
install(FILES themes/default.json DESTINATION share/snipline/themes)
install(FILES config.example.json DESTINATION share/snipline)
install(FILES packaging/snipline.desktop DESTINATION share/applications)
install(FILES
    protocols/wlr-foreign-toplevel-management-unstable-v1.xml
    "${WAYLAND_PROTOCOLS_DIR}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml"
    "${HYPRLAND_PROTOCOLS_DIR}/protocols/hyprland-toplevel-export-v1.xml"
    "${HYPRLAND_PROTOCOLS_DIR}/protocols/hyprland-toplevel-mapping-v1.xml"
    DESTINATION share/snipline/protocols)
