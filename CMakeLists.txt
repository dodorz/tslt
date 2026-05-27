cmake_minimum_required(VERSION 3.20)
project(tslt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tslt WIN32
    src/main.cpp
    src/AppConfig.cpp
    src/MainWindow.cpp
    src/TranslatorClient.cpp
)

target_include_directories(tslt PRIVATE src)

target_compile_definitions(tslt PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(tslt PRIVATE winhttp user32 gdi32 comctl32 shell32)

if(MINGW)
    target_link_options(tslt PRIVATE -municode)
endif()
