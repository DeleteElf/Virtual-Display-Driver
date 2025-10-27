@echo off

REM Retrieve from the registry
for /f "delims=" %%i in ('reg query "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v "DisplayVersion"') do set "DisplayVersion=%%i"
echo window display version: %DisplayVersion%

REM Take the content of the data section
for %%a in (%DisplayVersion%) do set "version=%%a"
echo version: %version%
REM data from ====> https://github.com/Drawbackz/DevCon-Installer/blob/master/devcon_sources.json
IF "%version%"=="23H2" set "WindowHash=C8FCE4377D8F0D184E5434F9EF1FE1EF9D0E34196A08CD7E5655555FABA21379"
IF "%version%"=="22H2" set "WindowHash=4F0C165C58114790DB7807872DEBD99379321024DB6077F0ED79426CF9C05CA0"
IF "%version%"=="21H2" set "WindowHash=FBD394E4407C6C334B933FF3A0D21A8E28F0EEDE0CFE5FB277287C3F994B5B00"

echo window hash:%WindowHash%

REM Write our configuration directory to the registry
rem install
set "DRIVER_DIR=%~dp0\VirtualDisplayDriver"
echo driver directory:%DRIVER_DIR%
rem Get root directory,current in the scripts directory.
for %%I in ("%~dp0\..\..") do set "ROOT_DIR=%%~fI"
set "CONFIG_DIR=%ROOT_DIR%\config"
set "VDD_CONFIG=%CONFIG_DIR%\vdd_settings.xml"
echo driver config path: %VDD_CONFIG%

REM It is always a good choice to copy configurations from the original directory to the configuration directory, whether for recovery or backup purposes
copy "%DRIVER_DIR%\vdd_settings.xml" "%VDD_CONFIG%"

REM The default read is the directory configured in the registry. We need to configure the directory in the registry
reg add "HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver" /v VDDPATH /t REG_SZ /d "%CONFIG_DIR%" /f

REM Install driver
powershell.exe -ExecutionPolicy Bypass -File "silent-install.ps1" $WindowHash

REM After the initial installation, it should also be set to expand the display mode of these monitors
powershell -NoProfile -ExecutionPolicy Bypass -Command "& "C:\Windows\System32\DisplaySwitch.exe" /extend"

REM After installation, disable the display driver first
powershell.exe -ExecutionPolicy Bypass -File "virtual-driver-manager.ps1" disable --silent true
pause