# Conway's Game of Life Screensaver

<img src="./preview.gif" width="500" />

## Building on Windows

0. Clone this repo to your machine. We'll assume it is at `~/conway`
1. Install C++ Bulding Tools for Windows in Visual Studio Installer
2. Open the "x64 Native Tools Command Prompt for VS 2022" CMD environment (or whichever version you have)
3. Clone `vcpkg`

```
git clone https://github.com/microsoft/vcpkg
cd vcpkg
bootstrap-vcpkg.bat
vcpkg install sdl2:x64-windows
cd ..
```

4. Build the `conway` application. This command will work without changes if your `conway` and `vcpkg` are siblings in the file tree. If that is not the case, change the `DCMAKE_TOOLCHAIN_FILE` argument, so that it has the right `vcpkg`-path.
```
cd conway

cmake -S . -B build ^
  -DCMAKE_TOOLCHAIN_FILE=..\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
```

5. Go to `~/conway/build/Release/`
6. Rename `ConwaySaver.exe` to `ConwaySaver.scr`
7. Right-click and select "install"