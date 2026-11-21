# Audio Enhancer

A native Windows audio enhancer built with Qt 6, CMake, Ninja, and the Windows WASAPI loopback API.

## Features

- Native Qt 6 interface
- WASAPI playback-device selection
- Normal, 3D, and Balanced DSP profiles
- Master volume, bass, and treble controls
- System-tray mode and Start with Windows
- Persistent user settings
- VB-Cable virtual-device detection

## Requirements

- Windows 10 or later
- CMake 3.25+
- Ninja
- MSYS2 UCRT64 GCC
- Qt 6 Widgets for the MSYS2 UCRT64 toolchain

The current preset assumes Qt is installed at `C:/msys64/ucrt64`. For another Qt installation,
set `CMAKE_PREFIX_PATH` when configuring.

## Build

Open an MSYS2 UCRT64 terminal in the repository root:

```bash
cmake --preset windows-release
cmake --build --preset windows-release
```

For a debug build:

```bash
cmake --preset windows-debug
cmake --build --preset windows-debug
```

The executable is written to `dist/Windows/AudioEnhancer.exe`.

Build the installer:

```bash
cmake --build --preset windows-release --target InnoSetup
```

The installer is written to `setup/output/AudioEnhancer-Setup.exe`.

## Run

```bash
./dist/Windows/AudioEnhancer.exe
```

To start directly in the system tray:

```bash
./dist/Windows/AudioEnhancer.exe --tray
```

## Project layout

- `src/main.cpp` — Qt application, settings, tray, and WASAPI integration
- `src/headers/audio_engine.h` — WASAPI capture/render engine
- `src/headers/dsp.h` — spatializer and tone-control DSP
- `src/app.rc` — Windows application icon and version metadata
- `src/assets/` — application icons
- `build/` — CMake/Ninja build trees
- `dist/Windows/` — generated executable
