@echo off
rem Builds the mod two ways:
rem   build\GestureSwitch.dll   for the crloader (F2G) mod loader: goes in the game's crmods\GestureSwitch\
rem                             folder, with menu\gestureswitch.menu.json (the CRModMenu descriptor)
rem   build\hid.dll             standalone: goes next to the game exe, no loader needed
rem build.bat deploy       copies GestureSwitch.dll and the descriptor into crmods\GestureSwitch\ (and removes
rem                        other copies of the mod, so it is not loaded twice)
rem build.bat deploy hid   copies hid.dll next to the game exe instead
rem build.bat package      makes the release zip, build\GestureSwitch-<version>.zip
setlocal
cd /d "%~dp0"
set GAME=D:\SteamLibrary\steamapps\common\CONTROL Resonant
set WINLIBS=%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin
where gcc >nul 2>nul || set "PATH=%WINLIBS%;%PATH%"

set SRC=src\main.c src\input.c src\xinput_hook.c src\ps_pad.c src\kbm.c src\gesture.c src\secondary.c src\finisher.c src\primary_chord.c src\power.c src\game.c src\damage.c src\scan.c src\hook.c src\config.c src\log.c
set FLAGS=-std=c11 -O2 -Wall -Wextra -shared -static -s

rem The menu descriptor must be strict JSON, or CRModMenu drops the whole MODS tab.
powershell -NoProfile -ExecutionPolicy Bypass -File menu\check_menu.ps1 menu\gestureswitch.menu.json || exit /b 1
rem The translated menu (menu\*.tsv: one file per language), checked the same way: it is the one installed.
if not exist build mkdir build
powershell -NoProfile -ExecutionPolicy Bypass -File menu\translate.ps1 -Src menu\gestureswitch.menu.json -Out build\gestureswitch.menu.json || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File menu\check_menu.ps1 build\gestureswitch.menu.json || exit /b 1
if not exist build mkdir build
gcc %FLAGS% -Wl,--exclude-all-symbols -o build\GestureSwitch.dll %SRC%
if errorlevel 1 exit /b 1
gcc %FLAGS% -DPROXY_HID -o build\hid.dll %SRC% src\proxy_hid.c src\hid.def
if errorlevel 1 exit /b 1
echo built build\GestureSwitch.dll and build\hid.dll

if /i "%1"=="package" goto package
if /i not "%1"=="deploy" exit /b 0

if /i "%2"=="hid" (
    if exist "%GAME%\hid.dll" if not exist "%GAME%\hid.dll.bak" (
        findstr /m /c:"GestureSwitch" "%GAME%\hid.dll" >nul || copy /y "%GAME%\hid.dll" "%GAME%\hid.dll.bak" >nul
    )
    copy /y build\hid.dll "%GAME%\hid.dll" >nul || exit /b 1
    echo deployed hid.dll to %GAME%
    exit /b 0
)

if not exist "%GAME%\crmods" (echo no crmods folder in %GAME%: install crloader first & exit /b 1)
set DEST=%GAME%\crmods\GestureSwitch
if not exist "%DEST%" mkdir "%DEST%"
copy /y build\GestureSwitch.dll "%DEST%\GestureSwitch.dll" >nul || exit /b 1
copy /y build\gestureswitch.menu.json "%DEST%\gestureswitch.menu.json" >nul || exit /b 1
rem Anything else that would load the mod a second time: the standalone build (only if it is ours), and the
rem mod straight in crmods\ from before it had its own folder.
if exist "%GAME%\hid.dll" findstr /m /c:"GestureSwitch" "%GAME%\hid.dll" >nul && del "%GAME%\hid.dll"
for %%f in (GestureSwitch.dll GestureSwitch.ini GestureSwitch.log) do if exist "%GAME%\crmods\%%f" del "%GAME%\crmods\%%f"
echo deployed to %DEST%
exit /b 0

:package
rem The release zip, laid out from the game folder down: players extract it into the folder with the exe.
rem Only the DLL, the menu descriptor and the docs: the ini and log are made on first run, and the loader and
rem the menu are separate downloads.
for /f "tokens=3" %%v in ('findstr /c:"#define MOD_VERSION" src\mod.h') do set VERSION=%%~v
set STAGE=build\package
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\crmods\GestureSwitch"
copy /y build\GestureSwitch.dll "%STAGE%\crmods\GestureSwitch\" >nul || exit /b 1
copy /y build\gestureswitch.menu.json "%STAGE%\crmods\GestureSwitch\" >nul || exit /b 1
copy /y README.md "%STAGE%\crmods\GestureSwitch\" >nul || exit /b 1
copy /y CREDITS.md "%STAGE%\crmods\GestureSwitch\" >nul || exit /b 1
if exist "build\GestureSwitch-%VERSION%.zip" del "build\GestureSwitch-%VERSION%.zip"
rem Windows' tar writes standard zip paths (forward slashes), which mod managers expect. Named in full: another tar
rem on the PATH (Git's) ignores -a and writes a tar file with a .zip name.
"%SystemRoot%\System32\tar.exe" -a -c -f "build\GestureSwitch-%VERSION%.zip" -C "%STAGE%" crmods || exit /b 1
powershell -NoProfile -Command "$f=[IO.File]::OpenRead('build\GestureSwitch-%VERSION%.zip'); $b=New-Object byte[] 2; [void]$f.Read($b,0,2); $f.Close(); if ($b[0] -ne 0x50 -or $b[1] -ne 0x4B) { 'package: not a zip file'; exit 1 }" || exit /b 1
echo packaged build\GestureSwitch-%VERSION%.zip
