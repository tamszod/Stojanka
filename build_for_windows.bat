@echo off
rem Builds Strojanka and produces dist\StrojankaInstaller.exe
rem Requirements: Visual Studio 2022 or newer with "Desktop development with C++", Git.
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "BUILD_DIR=build\windows"
set "TRIPLET=x64-windows"

rem ---- 1. Visual Studio C++ toolchain --------------------------------------------------
where cl >nul 2>nul
if not errorlevel 1 goto have_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_msvc
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL goto no_msvc
echo Using Visual Studio at %VSINSTALL%
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto no_msvc
:have_msvc

rem ---- 2. vcpkg (dependency manager) ---------------------------------------------------
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" goto have_vcpkg
if defined VCINSTALLDIR if exist "%VCINSTALLDIR%vcpkg\scripts\buildsystems\vcpkg.cmake" (
    set "VCPKG_ROOT=%VCINSTALLDIR%vcpkg"
    goto have_vcpkg
)
if not exist ".vcpkg\vcpkg.exe" (
    echo vcpkg not found, downloading it into .vcpkg ...
    git clone https://github.com/microsoft/vcpkg .vcpkg || goto failed
    call .vcpkg\bootstrap-vcpkg.bat -disableMetrics || goto failed
)
set "VCPKG_ROOT=%CD%\.vcpkg"
:have_vcpkg
echo Using vcpkg at %VCPKG_ROOT%

rem ---- 3. Configure, build, test, pack -------------------------------------------------
cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=%TRIPLET%
if errorlevel 1 goto failed

cmake --build "%BUILD_DIR%"
if errorlevel 1 goto failed

ctest --test-dir "%BUILD_DIR%" --output-on-failure
if errorlevel 1 goto failed

cmake --build "%BUILD_DIR%" --target dist
if errorlevel 1 goto failed

echo.
echo Done: %CD%\dist\StrojankaInstaller.exe
exit /b 0

:no_msvc
echo ERROR: Visual Studio with the C++ workload was not found.
echo Install "Desktop development with C++" from https://visualstudio.microsoft.com/
exit /b 1

:failed
echo.
echo BUILD FAILED
exit /b 1
