@echo off
REM Setup build environment for esp-modem-link
REM Usage: call scripts\setup_env.bat

REM CMake (from ESP-IDF toolchain)
set "CMAKE_PATH=D:\ESP32\Espressif\tools\cmake\3.30.2\bin"
set "PATH=%CMAKE_PATH%;%PATH%"

REM Ninja (from ESP-IDF toolchain)
set "NINJA_PATH=D:\ESP32\Espressif\tools\ninja\1.12.1"
if exist "%NINJA_PATH%" set "PATH=%NINJA_PATH%;%PATH%"

REM MSVC (Visual Studio 2022 Community)
set "VCVARSALL=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if exist "%VCVARSALL%" (
    call "%VCVARSALL%" x64 >nul
)

echo === Environment ready ===
where cmake
where cl
echo.
