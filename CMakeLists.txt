cmake_minimum_required(VERSION 3.20)
project(tslt VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tslt WIN32
    src/main.cpp
    src/AppConfig.cpp
    src/MainWindow.cpp
    src/TranslatorClient.cpp
    src/version.rc
)

target_include_directories(tslt PRIVATE src)

target_compile_definitions(tslt PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    TSLT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    TSLT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    TSLT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_link_libraries(tslt PRIVATE winhttp user32 gdi32 comctl32 shell32)

if(MINGW)
    target_link_options(tslt PRIVATE -municode)
endif()
