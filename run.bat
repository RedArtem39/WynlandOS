@echo off
rem WynlandOS - build the image and boot it in a QEMU window (no tests).
rem   run.bat              1600x900
rem   run.bat 1920x1080    another screen size
rem Needs WSL with the build tools (see the Makefile). Closing the QEMU
rem window ends it. The disk starts fresh on every build: first boot, a
rem new account.
setlocal
set RES=%1
if "%RES%"=="" set RES=1600x900
cd /d "%~dp0"

echo === WynlandOS: building (a few minutes the first time)...
wsl --cd "%~dp0." -e bash -lc "pkill -f '^qemu-system' 2>/dev/null; make all AUTOTEST=0 SNAPSHOT=0 BOOT_EXTRA= RESOLUTION=%RES% > build/run-bat.log 2>&1 || { tail -30 build/run-bat.log; exit 1; }"
if errorlevel 1 (
    echo === The build failed: build\run-bat.log
    pause
    exit /b 1
)

echo === Starting WynlandOS (%RES%). Ctrl+Alt+G lets the mouse go.
call :boot
if not errorlevel 1 goto :eof

rem QEMU's OpenGL under WSLg sometimes dies at start ("Couldn't find
rem current GLX or EGL context"): WSL once more, then again
echo === QEMU stopped with an error: restarting WSL and trying once more...
wsl --shutdown
call :boot
if errorlevel 1 pause
goto :eof

:boot
wsl --cd "%~dp0." -e bash -lc "make run-gl SMP=4 MEM=4096M AUTOTEST=0 SNAPSHOT=0 BOOT_EXTRA= UI=sdl RESOLUTION=%RES%"
exit /b %errorlevel%
