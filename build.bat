@echo off
rem Build JieTie.exe (截贴) with MinGW-w64 g++
rem Output: bin\JieTie.exe
rem Default GXX points at the local MinGW install; falls back to g++ in PATH.

set GXX=B:\mingw64\bin\g++.exe
if not exist "%GXX%" set GXX=g++

set SRC=%~dp0src\JieTie.cpp
set OUTDIR=%~dp0bin
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

"%GXX%" -municode -mwindows -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -finput-charset=UTF-8 "%SRC%" -o "%OUTDIR%\JieTie.exe" -lgdiplus -lshlwapi -lshell32 -lole32 -luuid -ladvapi32 -lgdi32

if exist "%OUTDIR%\JieTie.exe" (echo Build OK: bin\JieTie.exe) else (echo Build FAILED)
pause
