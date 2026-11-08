cmake_minimum_required(VERSION 3.20)
project(tslt VERSION 0.0.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# Portable tests for the platform independent JSON parser, protocol helpers and
# configuration key validation.
option(TSLT_BUILD_TESTS "Build the portable unit tests" OFF)
if(TSLT_BUILD_TESTS)
    enable_testing()
    foreach(test_name IN ITEMS json_protocol config_keys api_hints)
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
