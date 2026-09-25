@echo off
call "D:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul
rc /nologo resource.rc
cl /nologo /O2 /MT /utf-8 /DUNICODE /D_UNICODE main.cpp resource.res ^
   /link /SUBSYSTEM:WINDOWS /OUT:MiniGeekUninstaller.exe
if exist MiniGeekUninstaller.exe (
  echo BUILD_OK
  dir MiniGeekUninstaller.exe | findstr /C:"MiniGeek"
) else (
  echo BUILD_FAILED
)
