@echo off
rem ===== Build Hogs media player (hogofwar.exe + hogs_thumb.dll) with w64devkit =====
rem Needs (see README.md "Building"):
rem   * w64devkit (MinGW-w64 GCC): set W64DEVKIT to its folder, or edit the default below
rem   * .\deps\SDL2-2.32.10\x86_64-w64-mingw32           (SDL2 2.32.10 MinGW development release)
rem   * .\deps\ffmpeg-n7.1-win64-gpl-shared               (BtbN FFmpeg-Builds n7.1 win64 gpl-shared)
rem The AI enhancer (hogs_enhance.exe) is built separately: enhancer_src\build_enh.sh
setlocal
cd /d "%~dp0"
if not defined W64DEVKIT set "W64DEVKIT=C:\AI Resources help from hogofwar\w64devkit"
rem gcc spawns 'as'/'ld' via PATH, so w64devkit\bin MUST be on PATH or it fails with "cannot execute 'as'".
set "DK=%W64DEVKIT%\bin"
set "PATH=%DK%;%PATH%"
set "DEPS=.\deps"
set "SDL=%DEPS%\SDL2-2.32.10\x86_64-w64-mingw32"
set "FF=%DEPS%\ffmpeg-n7.1-win64-gpl-shared"

rem the 3D hog logo (hogs.ico) becomes the exe's icon: files/shortcuts/taskbar show it
"%DK%\windres.exe" hogs.rc -O coff -o hogs_res.o
if errorlevel 1 ( echo ICON RESOURCE FAILED & pause & exit /b 1 )
"%DK%\gcc.exe" hogs_ultimate.c hogs_res.o -O2 -o hogofwar.exe ^
  -I "%SDL%\include\SDL2" -I "%FF%\include" ^
  -L "%SDL%\lib" -L "%FF%\lib" ^
  -lmingw32 -lSDL2 -lavformat -lavcodec -lavutil -lswscale -lswresample ^
  -lopengl32 -lcomdlg32 -lole32 -lshell32 -ldbghelp -lm -mwindows
if errorlevel 1 ( echo BUILD FAILED & pause & exit /b 1 )
rem Explorer thumbnail provider (decodes nothing itself: runs hogofwar.exe --thumb)
rem Explorer keeps the registered DLL loaded, so it can't be overwritten - but it CAN be renamed aside.
"%DK%\gcc.exe" -shared -O2 hogs_thumb.c -o hogs_thumb.new.dll -lole32 -luuid -lgdi32 -s
if errorlevel 1 ( echo THUMBNAIL DLL FAILED & pause & exit /b 1 )
del /q hogs_thumb.old.dll 2>nul
if exist hogs_thumb.dll ren hogs_thumb.dll hogs_thumb.old.dll
ren hogs_thumb.new.dll hogs_thumb.dll

rem ship the runtime DLLs next to the exe (refresh in case deps changed)
copy /y "%FF%\bin\*.dll" . >nul
copy /y "%SDL%\bin\SDL2.dll" . >nul
echo BUILD OK -^> hogofwar.exe
