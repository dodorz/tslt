cmake_minimum_required(VERSION 3.20)

# Release builds pass -DTSLT_VERSION=<version derived from the git tag>, so the
# executable always reports the tag it was built from. Local builds fall back to
# the last released version; bump TSLT_VERSION_DEFAULT when a new tag is pushed.
set(TSLT_VERSION_DEFAULT "0.0.10")

# Only set by the release workflow (-DTSLT_VERSION=<tag without the leading v>).
set(TSLT_VERSION "" CACHE STRING "Version embedded in the executable; set by the release workflow")
if(TSLT_VERSION STREQUAL "")
    set(TSLT_VERSION "${TSLT_VERSION_DEFAULT}")
elseif(NOT TSLT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
    # CMake's regex engine has no {n,m} repetition, and project(VERSION ...) plus the
    # resource script only understand numeric components. Failing here is deliberate:
    # silently falling back would ship a binary that reports the wrong version.
    message(FATAL_ERROR "TSLT_VERSION='${TSLT_VERSION}' is not a dotted numeric version (X.Y.Z or X.Y.Z.W)")
endif()

project(tslt VERSION "${TSLT_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

message(STATUS "tslt version: ${PROJECT_VERSION}")

# The resource script needs all four numeric components; a three-part version
# (the common case) leaves PROJECT_VERSION_TWEAK empty.
set(TSLT_FILE_VERSION_TWEAK "${PROJECT_VERSION_TWEAK}")
if(TSLT_FILE_VERSION_TWEAK STREQUAL "")
    set(TSLT_FILE_VERSION_TWEAK "0")
endif()

configure_file(src/version.rc.in version.rc @ONLY)

add_executable(tslt WIN32
    src/main.cpp
    src/AppConfig.cpp
    src/MainWindow.cpp
    src/TranslatorClient.cpp
    src/DictionaryClient.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)

target_include_directories(tslt PRIVATE src)

target_compile_definitions(tslt PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    # Sources contain UTF-8 literals; keep the compiler from interpreting them with the system code page.
    target_compile_options(tslt PRIVATE /utf-8)
endif()

target_link_libraries(tslt PRIVATE winhttp user32 gdi32 comctl32 shell32 uxtheme)

if(MINGW)
    target_link_options(tslt PRIVATE -municode)
endif()

# Portable tests for the platform independent JSON parser, protocol helpers,
# configuration key validation and error classification.
option(TSLT_BUILD_TESTS "Build the portable unit tests" OFF)
if(TSLT_BUILD_TESTS)
    enable_testing()
    foreach(test_name IN ITEMS json_protocol config_keys api_hints winhttp_errors sse)
        add_executable(tslt_${test_name}_tests tests/${test_name}_tests.cpp)
        target_include_directories(tslt_${test_name}_tests PRIVATE src)
        set_target_properties(tslt_${test_name}_tests PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(MSVC)
            target_compile_options(tslt_${test_name}_tests PRIVATE /utf-8)
        endif()
        add_test(NAME tslt_${test_name}_tests COMMAND tslt_${test_name}_tests)
    endforeach()
endif()
