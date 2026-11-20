cmake_minimum_required(VERSION 3.24)
project(snipline VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
include(CTest)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2 DBus)
find_package(LayerShellQt REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Wayland REQUIRED IMPORTED_TARGET wayland-client)
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
    src/core/theme.cpp src/core/exporter.cpp src/core/outputs.cpp src/core/windowtargets.cpp)
target_include_directories(snipline_core PUBLIC src)
target_link_libraries(snipline_core PUBLIC Qt6::Core Qt6::Gui)
target_compile_options(snipline_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(snipline_app STATIC src/capture.cpp src/controller.cpp src/windows.cpp src/surfacebarrier.cpp src/windowpicker.cpp)
target_link_libraries(snipline_app PUBLIC snipline_core Qt6::Quick
    Qt6::QuickControls2 Qt6::DBus LayerShellQt::Interface snipline_protocols)
target_compile_options(snipline_app PRIVATE -Wall -Wextra -Wpedantic)
qt_add_executable(snipline src/main.cpp src/doctor.cpp)
qt_add_resources(snipline ui PREFIX "/" FILES qml/Overlay.qml)
target_link_libraries(snipline PRIVATE snipline_app PkgConfig::Wayland)
target_compile_options(snipline PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(snipline_tests tests/core_test.cpp)
    target_link_libraries(snipline_tests PRIVATE snipline_core Qt6::Test)
    add_test(NAME core COMMAND snipline_tests)
    qt_add_executable(snipline_target_tests tests/windowtargets_test.cpp)
    target_link_libraries(snipline_target_tests PRIVATE snipline_core Qt6::Test)
    add_test(NAME window_targets COMMAND snipline_target_tests)
    set_tests_properties(core PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(snipline_ui_tests tests/ui_test.cpp)
    qt_add_resources(snipline_ui_tests test_ui PREFIX "/" FILES qml/Overlay.qml)
    target_link_libraries(snipline_ui_tests PRIVATE snipline_app Qt6::Test)
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
        qt_add_executable(snipline_instance_tests tests/instance_test.cpp)
        target_link_libraries(snipline_instance_tests PRIVATE Qt6::Test Qt6::DBus)
        add_test(NAME instance COMMAND ${DBUS_RUN_SESSION} --
            ${CMAKE_COMMAND} -E env "SNIPLINE_BINARY=$<TARGET_FILE:snipline>"
            "QT_QPA_PLATFORM=offscreen" $<TARGET_FILE:snipline_instance_tests>)
    endif()
endif()

install(TARGETS snipline RUNTIME DESTINATION bin)
install(FILES themes/default.json DESTINATION share/snipline/themes)
install(FILES packaging/snipline.desktop DESTINATION share/applications)
install(FILES
    protocols/wlr-foreign-toplevel-management-unstable-v1.xml
    "${WAYLAND_PROTOCOLS_DIR}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml"
    "${HYPRLAND_PROTOCOLS_DIR}/protocols/hyprland-toplevel-export-v1.xml"
    "${HYPRLAND_PROTOCOLS_DIR}/protocols/hyprland-toplevel-mapping-v1.xml"
    DESTINATION share/snipline/protocols)
