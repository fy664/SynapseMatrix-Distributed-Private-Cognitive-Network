@echo off
REM SynapseMatrix - C++ Runtime build script (Month 3)
REM Usage: cmd /c build_runtime.bat
REM Calls MSVC vcvars64 then configures+builds runtime/ with CMake.
setlocal
cd /d "%~dp0"
call "D:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
echo [1/3] vcvars64 OK
REM 注意：PATH 里 cmake 是 cmake.cmd shim，bat 内必须 call，
REM 否则控制权不返回、后续行被吞（本机踩过的坑）。
call cmake -S runtime -B runtime/build || exit /b 1
echo [2/3] cmake configure OK
call cmake --build runtime/build --config Release || exit /b 1
echo [3/3] build OK. Binaries under runtime\build\Release\
endlocal

