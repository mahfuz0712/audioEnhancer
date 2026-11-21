cmake_minimum_required(VERSION 3.25)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" AE_METADATA_JSON)
string(JSON AE_APP_NAME GET "${AE_METADATA_JSON}" name)
string(JSON AE_APP_VERSION GET "${AE_METADATA_JSON}" version)

if(AE_APP_NAME STREQUAL "" OR AE_APP_VERSION STREQUAL "")
    message(FATAL_ERROR "metadata.json must contain name and version")
endif()

string(REPLACE "." ";" AE_VERSION_PARTS "${AE_APP_VERSION}")
list(LENGTH AE_VERSION_PARTS AE_VERSION_PART_COUNT)
if(NOT AE_VERSION_PART_COUNT EQUAL 3)
    message(FATAL_ERROR "metadata.json version must use MAJOR.MINOR.PATCH format")
endif()
list(GET AE_VERSION_PARTS 0 AE_VERSION_MAJOR)
list(GET AE_VERSION_PARTS 1 AE_VERSION_MINOR)
list(GET AE_VERSION_PARTS 2 AE_VERSION_PATCH)
string(REPLACE " " "-" AE_APP_SLUG "${AE_APP_NAME}")
set(AE_APP_ARCH "x64")

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
file(WRITE "${CMAKE_BINARY_DIR}/generated/version.h"
"// Generated from metadata.json. Do not edit manually.\n"
"#pragma once\n"
"#define APP_NAME_STR \"${AE_APP_NAME}\"\n"
"#define APP_VERSION_MAJOR ${AE_VERSION_MAJOR}\n"
"#define APP_VERSION_MINOR ${AE_VERSION_MINOR}\n"
"#define APP_VERSION_PATCH ${AE_VERSION_PATCH}\n"
"#define APP_VERSION_STR \"${AE_APP_VERSION}\"\n"
)

project(AudioEnhancer
    VERSION "${AE_APP_VERSION}"
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
    src/headers/audio_engine.h
    src/headers/dsp.h
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
    "${CMAKE_BINARY_DIR}/generated"
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

find_program(WINDEPLOYQT_EXECUTABLE windeployqt6 HINTS "C:/msys64/ucrt64/bin")
if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET AudioEnhancer POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --compiler-runtime --no-translations --no-opengl-sw
                --dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/AudioEnhancer.exe"
        COMMENT "Deploying Qt 6 runtime beside AudioEnhancer.exe"
    )
endif()

if(MSVC)
    target_compile_options(AudioEnhancer PRIVATE /W4 /permissive-)
else()
    target_compile_options(AudioEnhancer PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_program(INNO_SETUP_COMPILER iscc HINTS
    "C:/Program Files (x86)/Inno Setup 6"
    "C:/Program Files/Inno Setup 6"
)

if(INNO_SETUP_COMPILER)
    add_custom_target(InnoSetup
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/setup/output"
        COMMAND "${INNO_SETUP_COMPILER}"
                "/DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "/DAPP_DIR=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                "/DOUTPUT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/setup/output"
                "/DAPP_VERSION=${AE_APP_VERSION}"
                "/DAPP_NAME=${AE_APP_NAME}"
                "/DAPP_SLUG=${AE_APP_SLUG}"
                "/DAPP_ARCH=${AE_APP_ARCH}"
                "${CMAKE_CURRENT_SOURCE_DIR}/setup/setup.iss"
        DEPENDS AudioEnhancer
        COMMENT "Building AudioEnhancer-Setup.exe with Inno Setup"
    )
else()
    add_custom_target(InnoSetup
        COMMAND ${CMAKE_COMMAND} -E echo
                "Inno Setup 6 is not installed. Install ISCC and build again."
    )
endif()
