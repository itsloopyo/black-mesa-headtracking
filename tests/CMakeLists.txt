# SPDX-License-Identifier: MIT
# Copyright (c) 2026 itsloopyo / CameraUnlock
# Characterization tests for the mod's pure logic. Deliberately links nothing
# from the game or the hooks: anything that needs a live client.dll is verified
# in-game, and anything that does not belongs here.
add_executable(bms_headtracking_tests
    test_main.cpp
    source_math_tests.cpp
    position_mapping_tests.cpp
    log_throttle_tests.cpp
    build_profile_tests.cpp
    zoom_scale_tests.cpp
    ${CMAKE_SOURCE_DIR}/src/source_math.cpp
    ${CMAKE_SOURCE_DIR}/src/builds/steam_offsets.cpp
)

# The core include dir is for header-only types only (PositionSettings, and
# PeFingerprint for the build profiles), so the suite still links nothing from
# the game or the hooks.
target_include_directories(bms_headtracking_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/cameraunlock-core/cpp/include
)

if(MSVC)
    target_compile_options(bms_headtracking_tests PRIVATE /W4 /WX)
else()
    target_compile_options(bms_headtracking_tests PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME bms_headtracking_tests COMMAND bms_headtracking_tests)

# CameraUnlock.ini: the committed file against the table, a first start, the toggles' saves and
# the legacy file's ANSI path. `--render-config <path>` makes it write the committed file
# instead (pixi run render-config).
add_executable(bms_config_tests
    config_tests.cpp
    ${CMAKE_SOURCE_DIR}/src/config.cpp
    ${CMAKE_SOURCE_DIR}/src/legacy_config/legacy_config.cpp
)
target_include_directories(bms_config_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bms_config_tests PRIVATE cameraunlock)
target_compile_definitions(bms_config_tests PRIVATE
    BMS_COMMITTED_CONFIG="${CMAKE_SOURCE_DIR}/config/CameraUnlock.ini")
if(MSVC)
    target_compile_options(bms_config_tests PRIVATE /W4 /WX)
else()
    target_compile_options(bms_config_tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME bms_config_tests COMMAND bms_config_tests)

# The differential test for the HeadTracking.ini reader. The oracle is the reader of the newest
# published build (v0.1.1), compiled from its own sources in config_differential/oracle with the
# mod's and core's namespaces renamed, so it links beside today's code and nothing in it can
# resolve to a symbol of the mod under test. Its include roots put the published build's files
# ahead of core's headers.
set(BMS_CORE_CPP ${CMAKE_SOURCE_DIR}/cameraunlock-core/cpp)
add_library(bms_config_oracle STATIC
    config_differential/oracle_api.cpp
    config_differential/oracle/src/config.cpp
    ${BMS_CORE_CPP}/src/config/ini_reader.cpp
    ${BMS_CORE_CPP}/src/logging/file_log.cpp
)
target_include_directories(bms_config_oracle PRIVATE
    config_differential/oracle/src
    ${BMS_CORE_CPP}/include
)
target_compile_definitions(bms_config_oracle PRIVATE
    headtracking=bms_oracle
    cameraunlock=bms_oracle_core
)

add_executable(bms_config_differential_tests
    config_differential/differential_tests.cpp
    ${CMAKE_SOURCE_DIR}/src/config.cpp
    ${CMAKE_SOURCE_DIR}/src/legacy_config/legacy_config.cpp
)
target_include_directories(bms_config_differential_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    config_differential
)
target_link_libraries(bms_config_differential_tests PRIVATE bms_config_oracle cameraunlock bcrypt)
target_compile_definitions(bms_config_differential_tests PRIVATE BMS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_compile_options(bms_config_differential_tests PRIVATE /W4 /WX)
else()
    target_compile_options(bms_config_differential_tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME bms_config_differential_tests COMMAND bms_config_differential_tests)

# Core's canonical config lint over the committed file and every file the differential test
# migrated, which it writes beside itself under migrated.
add_test(NAME bms_config_lint COMMAND node
    ${CMAKE_CURRENT_SOURCE_DIR}/config_differential/lint-migrated.mjs
    $<TARGET_FILE_DIR:bms_config_differential_tests>/migrated)
set_tests_properties(bms_config_lint PROPERTIES DEPENDS bms_config_differential_tests)
