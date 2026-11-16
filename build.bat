@echo off
rem Build JieTie.exe with MinGW-w64 g++
rem Default path points at the local MinGW install; falls back to g++ in PATH.
set GXX=B:\mingw64\bin\g++.exe
if not exist "%GXX%" set GXX=g++

"%GXX%" -municode -mwindows -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -finput-charset=UTF-8 "%~dp0JieTie.cpp" -o "%~dp0JieTie.exe" -lgdiplus -lshlwapi -lshell32 -lole32 -luuid -ladvapi32 -lgdi32
if exist "%~dp0JieTie.exe" (echo Build OK: JieTie.exe) else (echo Build FAILED)
pause
