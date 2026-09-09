@echo off
setlocal

:: AT168Shot CMake Build Script
:: Builds the application using CMake (requires CMake and a C++ compiler)

echo AT168Shot Build Script
echo ====================

where cmake >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake not found!
    echo Please install CMake from https://cmake.org/download/
    exit /b 1
)

echo.
echo Configuring...
cmake -B build -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% neq 0 (
    echo.
    echo Configuration FAILED!
    exit /b 1
)

echo.
echo Building...
cmake --build build --config Release
if %ERRORLEVEL% neq 0 (
    echo.
    echo Build FAILED!
    exit /b 1
)

echo.
echo Build successful!
if exist build\Release\at168shot.exe (
    for %%A in (build\Release\at168shot.exe) do echo Output: build\Release\at168shot.exe (%%~zA bytes)
) else if exist build\at168shot.exe (
    for %%A in (build\at168shot.exe) do echo Output: build\at168shot.exe (%%~zA bytes)
)

echo.
echo Done!
