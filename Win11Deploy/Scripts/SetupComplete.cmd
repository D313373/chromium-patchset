@echo off
setlocal ENABLEEXTENSIONS
REM Runs once after Windows specialize, before first logon.
REM Requires the deploy USB (models.json + Drivers) to still be plugged in.

set LOGDIR=%SystemRoot%\Setup\Scripts
if not exist "%LOGDIR%" mkdir "%LOGDIR%"
echo %DATE% %TIME% SetupComplete.cmd starting>>"%LOGDIR%\model-detect.log"

REM Locate deploy USB by models.json marker (drive letters C-Z).
set "USBROOT="
for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
  if exist "%%D:\models.json" if exist "%%D:\Drivers" if exist "%%D:\Scripts\Run-ModelAwareSetup.ps1" (
    set "USBROOT=%%D:"
    goto :found
  )
)

echo %DATE% %TIME% ERROR: deploy USB not found>>"%LOGDIR%\model-detect.log"
exit /b 1

:found
echo %DATE% %TIME% Using USB %USBROOT%>>"%LOGDIR%\model-detect.log"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%USBROOT%\Scripts\Run-ModelAwareSetup.ps1" -UsbRoot "%USBROOT%"
set ERR=%ERRORLEVEL%
echo %DATE% %TIME% Run-ModelAwareSetup.ps1 exit %ERR%>>"%LOGDIR%\model-detect.log"
exit /b %ERR%
