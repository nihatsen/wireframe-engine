@echo off
setlocal EnableDelayedExpansion

echo ===============================================
echo  Installing ZygiskNext and Wireframe Engine
echo  via KernelSU (ksud)
echo ===============================================
echo.

set ADB=adb.exe
set ZYGISK_NEXT=Zygisk-Next-1.3.3.zip
set WIREFRAME=wireframe-engine-v1.0.0-20260407.zip

REM Check if device is connected
echo Checking device connection...
%ADB% devices | findstr "device$" >nul
if errorlevel 1 (
    echo ERROR: No device connected!
    echo Please connect your phone and enable USB debugging.
    pause
    exit /b 1
)

echo Device connected.
echo.

REM Check if KernelSU is installed
echo Checking KernelSU installation...
%ADB% shell "su -c 'ksud --version'" 2>nul
if errorlevel 1 (
    echo WARNING: KernelSU not detected or ksud not found.
    echo Make sure KernelSU is properly installed.
)
echo.

REM Push ZygiskNext module to device
echo Pushing %ZYGISK_NEXT% to device...
%ADB% push "%~dp0%ZYGISK_NEXT%" /data/local/tmp/%ZYGISK_NEXT%
if errorlevel 1 (
    echo ERROR: Failed to push %ZYGISK_NEXT%
    pause
    exit /b 1
)
echo Done.
echo.

REM Push Wireframe Engine module to device
echo Pushing %WIREFRAME% to device...
%ADB% push "%~dp0out\%WIREFRAME%" /data/local/tmp/%WIREFRAME%
if errorlevel 1 (
    echo ERROR: Failed to push %WIREFRAME%
    pause
    exit /b 1
)
echo Done.
echo.

REM Install ZygiskNext first
echo ===============================================
echo  Installing ZygiskNext...
echo ===============================================
%ADB% shell "su -c 'ksud module install /data/local/tmp/%ZYGISK_NEXT%'"
if errorlevel 1 (
    echo WARNING: Installation may have failed or module already exists.
) else (
    echo ZygiskNext installed successfully.
)
echo.

REM Install Wireframe Engine
echo ===============================================
echo  Installing Wireframe Engine...
echo ===============================================
%ADB% shell "su -c 'ksud module install /data/local/tmp/%WIREFRAME%'"
if errorlevel 1 (
    echo WARNING: Installation may have failed or module already exists.
) else (
    echo Wireframe Engine installed successfully.
)
echo.

REM Clean up temp files
echo Cleaning up temporary files...
%ADB% shell "rm /data/local/tmp/%ZYGISK_NEXT%"
%ADB% shell "rm /data/local/tmp/%WIREFRAME%"
echo Done.
echo.

REM List installed modules
echo ===============================================
echo  Installed Modules:
echo ===============================================
%ADB% shell "su -c 'ksud module list'"
echo.

REM Ask to reboot
echo ===============================================
echo  Installation Complete!
echo ===============================================
echo.
echo Reboot required for modules to take effect.
set /p REBOOT="Reboot now? (y/n): "
if /i "%REBOOT%"=="y" (
    echo Rebooting device...
    %ADB% reboot
) else (
    echo Please reboot manually when ready.
)

pause
