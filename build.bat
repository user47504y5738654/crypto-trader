@echo off
set PATH=C:\msys64\ucrt64\bin;C:\msys64\usr\bin;%PATH%
set CC=gcc
set CXX=g++
cd /d C:\Users\admin\crypto-trader
if exist build rmdir /s /q build
mkdir build
cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ 2>&1
if %ERRORLEVEL% NEQ 0 exit /b 1
mingw32-make -j4 2>&1
