@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
python "%~dp0tools\gen_test_main.py" || exit /b 1
cmake -G Ninja -S "%~dp0." -B "%~dp0build" >nul || exit /b 1
cmake --build "%~dp0build" || exit /b 1
"%~dp0build\cardos_tests.exe"
