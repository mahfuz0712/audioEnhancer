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
