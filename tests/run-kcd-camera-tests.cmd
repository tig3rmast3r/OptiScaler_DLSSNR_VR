@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /EHsc /std:c++20 /O2 /W4 /WX kcd_camera_packet_tests.cpp /Fo"build\kcd_camera_packet_tests.obj" /Fe"build\kcd_camera_packet_tests.exe"
if errorlevel 1 exit /b 1
"build\kcd_camera_packet_tests.exe"
exit /b %errorlevel%
