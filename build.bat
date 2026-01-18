@echo off
setlocal

:: RadShot C++ Build Script
:: Builds a minimal-size executable using MSVC

echo RadShot Build Script
echo ====================

:: Try to find Visual Studio
set "VCVARS="

:: VS 2022 (Program Files)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)

:: VS 2022 (Program Files x86)
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
)
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
)
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
)
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)

:: VS 2019
if "%VCVARS%"=="" (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    )
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
    )
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    )
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if "%VCVARS%"=="" (
    echo ERROR: Visual Studio not found!
    echo Please install Visual Studio 2019 or 2022 with C++ build tools.
    exit /b 1
)

echo Using: %VCVARS%
call "%VCVARS%" >nul 2>&1

:: Create icon file if needed
if not exist radshot.ico (
    echo.
    echo Creating application icon...
    where python >nul 2>&1
    if %ERRORLEVEL% equ 0 (
        python create_icon.py
        if %ERRORLEVEL% neq 0 (
            echo WARNING: Failed to create icon. Building without icon.
            goto :skip_icon
        )
    ) else (
        echo WARNING: Python not found. Cannot create icon from PNG.
        echo          Install Python and Pillow, or provide radshot.ico manually.
        goto :skip_icon
    )
)

:: Compile resource file
if exist radshot.ico (
    echo.
    echo Compiling resources...
    rc /nologo /fo radshot.res radshot.rc
    if %ERRORLEVEL% neq 0 (
        echo WARNING: Resource compilation failed. Building without icon.
        goto :skip_icon
    )
    set "RES_FILE=radshot.res"
) else (
    :skip_icon
    set "RES_FILE="
)

:: Compiler flags for minimal size
set CFLAGS=/nologo /O1 /GS- /GL /DNDEBUG /D_CRT_SECURE_NO_WARNINGS
set CFLAGS=%CFLAGS% /EHsc /MT
set CFLAGS=%CFLAGS% /I. /Iimgui
set CFLAGS=%CFLAGS% /wd4244 /wd4267 /wd4305

:: Linker flags for minimal size
set LDFLAGS=/LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS

:: Libraries
set LIBS=opengl32.lib user32.lib gdi32.lib setupapi.lib advapi32.lib comdlg32.lib shell32.lib ole32.lib

:: Source files
set SOURCES=radshot.cpp
set SOURCES=%SOURCES% imgui/imgui.cpp imgui/imgui_draw.cpp
set SOURCES=%SOURCES% imgui/imgui_tables.cpp imgui/imgui_widgets.cpp
set SOURCES=%SOURCES% imgui/imgui_impl_win32.cpp imgui/imgui_impl_opengl3.cpp

echo.
echo Compiling...
cl %CFLAGS% %SOURCES% /Fe:radshot.exe /link %LDFLAGS% %LIBS% %RES_FILE%

if %ERRORLEVEL% neq 0 (
    echo.
    echo Build FAILED!
    exit /b 1
)

:: Clean up intermediate files
del *.obj 2>nul
del *.res 2>nul

:: Show result
echo.
echo Build successful!
for %%A in (radshot.exe) do echo Output: radshot.exe (%%~zA bytes)

:: Optional: UPX compression
where upx >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo.
    echo Compressing with UPX...
    upx --best --lzma radshot.exe
    for %%A in (radshot.exe) do echo Compressed: radshot.exe (%%~zA bytes)
) else (
    echo.
    echo Note: Install UPX for additional compression
    echo       https://github.com/upx/upx/releases
)

echo.
echo Done!
