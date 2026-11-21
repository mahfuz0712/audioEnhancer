cmake_minimum_required(VERSION 3.25)

project(AudioEnhancer
    VERSION 1.0.3
    DESCRIPTION "WASAPI audio enhancer with a native Qt 6 interface"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "Audio Enhancer uses the Windows WASAPI API and can only be built on Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist/Windows")

add_executable(AudioEnhancer WIN32
    src/main.cpp
    src/resources/app.rc
    src/resources.qrc
)

set_property(SOURCE src/resources/app.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/app.ico"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/app_off.ico"
)

target_include_directories(AudioEnhancer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/headers"
)

target_link_libraries(AudioEnhancer PRIVATE
    Qt6::Widgets
    Qt6::Network
    ole32
    oleaut32
    uuid
    winmm
    gdi32
    user32
    shell32
    advapi32
)

set_target_properties(AudioEnhancer PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    OUTPUT_NAME "AudioEnhancer"
    WIN32_EXECUTABLE ON
)

if(MSVC)
    target_compile_options(AudioEnhancer PRIVATE /W4 /permissive-)
else()
    target_compile_options(AudioEnhancer PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(AudioEnhancerSetup WIN32
    setup/setup.cpp
    src/resources/setup.rc
)

target_include_directories(AudioEnhancerSetup PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/headers"
)

target_link_libraries(AudioEnhancerSetup PRIVATE
    ole32
    oleaut32
    uuid
    winmm
    comctl32
    urlmon
    shell32
    advapi32
    user32
    gdi32
)

set_target_properties(AudioEnhancerSetup PROPERTIES
    OUTPUT_NAME "AudioEnhancer-Setup"
)

target_compile_definitions(AudioEnhancerSetup PRIVATE UNICODE _UNICODE)
add_dependencies(AudioEnhancerSetup AudioEnhancer)
