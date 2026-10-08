# Audio Enhancer - build with MSYS2 (UCRT64 terminal)
#   pacman -S --needed mingw-w64-ucrt-x86_64-gcc make
#   make          -> dist/AudioEnhancer.exe  (+ dist/ui + dist/WebView2Loader.dll)
#   make run      -> build + run
#   make setup    -> release/AudioEnhancer-Setup.exe  (single-file installer)
#   make clean
#
# Needs:
#   src/headers/WebView2.h        (from the Microsoft.Web.WebView2 NuGet package)
#   libs/WebView2Loader.dll       (x64 DLL from the same package)
#   assets/app.ico                (app icon)

CXX      := g++
WINDRES  := windres
TARGET   := dist/AudioEnhancer.exe
SETUP    := release/AudioEnhancer-Setup.exe
OBJ      := build/main.o build/app_res.o
HEADERS  := $(wildcard src/headers/*.h)

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unknown-pragmas -DUNICODE -D_UNICODE -Isrc/headers
LDFLAGS  := -mwindows -static -static-libgcc -static-libstdc++
LDLIBS   := -lole32 -loleaut32 -luuid -lwinmm -lgdi32 -luser32 -lshell32 -ladvapi32
SETUPLIBS := -lole32 -loleaut32 -luuid -lwinmm -lcomctl32 -lurlmon -lshell32 -ladvapi32 -luser32 -lgdi32

# Use ">" instead of TAB for recipe lines
.RECIPEPREFIX = >
.PHONY: all assets run setup clean

all: $(TARGET) assets

$(TARGET): $(OBJ)
> @mkdir -p dist
> $(CXX) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)
> @echo Built $@

build/%.o: src/%.cpp $(HEADERS)
> @mkdir -p build
> $(CXX) $(CXXFLAGS) -c $< -o $@

# App icon resource (src/app.rc -> build/app_res.o)
build/app_res.o: src/app.rc assets/app.ico assets/app_off.ico
> @mkdir -p build
> $(WINDRES) -i src/app.rc -O coff -o $@

# Copies the HTML/CSS/JS UI and the WebView2 loader next to the exe
assets:
> @mkdir -p dist
> @rm -rf dist/ui
> @cp -r ui dist/ui
> @cp -f libs/WebView2Loader.dll dist/WebView2Loader.dll

# Single-file installer: embeds dist/AudioEnhancer.exe, the DLL, the UI and the icon
setup: all
> @mkdir -p build release
> $(WINDRES) -i src/setup.rc -O coff -o build/setup_res.o
> $(CXX) $(CXXFLAGS) -c src/setup.cpp -o build/setup.o
> $(CXX) build/setup.o build/setup_res.o -o $(SETUP) $(LDFLAGS) $(SETUPLIBS)
> @echo Built $(SETUP)

run: all
> ./$(TARGET)

clean:
> rm -rf build dist release