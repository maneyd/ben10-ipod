cmake_minimum_required(VERSION 3.20)
project(resona_player LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sysroot/usr/lib/x86_64-linux-gnu")
    set(CMAKE_BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sysroot/usr/lib/x86_64-linux-gnu")
    set(CMAKE_INSTALL_RPATH "${CMAKE_BUILD_RPATH}")
    set(SYSROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sysroot/usr")
    include_directories("${SYSROOT}/include" "${SYSROOT}/include/x86_64-linux-gnu")
    link_directories("${SYSROOT}/lib/x86_64-linux-gnu")
    set(ENV{PKG_CONFIG_PATH} "${SYSROOT}/lib/x86_64-linux-gnu/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(DBUS1 IMPORTED_TARGET dbus-1)

set(CORE_SOURCES
    src/domain/models.cpp
    src/storage/paths.cpp
    src/storage/json_store.cpp
    src/storage/library_store.cpp
    src/local/scanner.cpp
    src/network/http.cpp
    src/youtube/visitor.cpp
    src/youtube/innertube.cpp
    src/youtube/extractor.cpp
    src/search/search_service.cpp
    src/audio/audio_engine.cpp
    src/player/player_controller.cpp
)

add_library(player_core STATIC ${CORE_SOURCES})
target_include_directories(player_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(SYSROOT)
    target_link_directories(player_core PUBLIC "${SYSROOT}/lib/x86_64-linux-gnu")
endif()
target_link_libraries(player_core PUBLIC
    curl
    z
    avformat
    avcodec
    avutil
    swresample
    pulse-simple
    pulse
    tag
    pthread
)

target_link_options(player_core INTERFACE "LINKER:--allow-shlib-undefined")

add_executable(player src/app/main.cpp)
target_link_libraries(player PRIVATE player_core)

add_executable(player_ui src/ui/main.cpp)
target_include_directories(player_ui PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/microui"
)
if(DBUS1_FOUND)
    target_sources(player_ui PRIVATE src/tray/status_notifier.cpp)
    target_compile_definitions(player_ui PRIVATE OMNITRIX_HAVE_DBUS=1)
    target_link_libraries(player_ui PRIVATE player_core PkgConfig::SDL2 PkgConfig::DBUS1)
else()
    target_link_libraries(player_ui PRIVATE player_core PkgConfig::SDL2)
endif()

add_executable(player_tests tests/unit_tests.cpp)
target_link_libraries(player_tests PRIVATE player_core)
enable_testing()
add_test(NAME unit_tests COMMAND player_tests)
