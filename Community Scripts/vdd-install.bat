@echo off

REM 从注册表获取
for /f "delims=" %%i in ('reg query "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v "DisplayVersion"') do set "DisplayVersion=%%i"
echo window display version: %DisplayVersion%

REM 取数据部分的内容
for %%a in (%DisplayVersion%) do set "version=%%a"
echo version: %version%
REM 数据来源：https://github.com/Drawbackz/DevCon-Installer/blob/master/devcon_sources.json
IF "%version%"=="23H2" set "WindowHash=C8FCE4377D8F0D184E5434F9EF1FE1EF9D0E34196A08CD7E5655555FABA21379"
IF "%version%"=="22H2" set "WindowHash=4F0C165C58114790DB7807872DEBD99379321024DB6077F0ED79426CF9C05CA0"
IF "%version%"=="21H2" set "WindowHash=FBD394E4407C6C334B933FF3A0D21A8E28F0EEDE0CFE5FB277287C3F994B5B00"

echo window hash:%WindowHash%

powershell.exe -ExecutionPolicy Bypass -File "silent-install.ps1" $WindowHash

pause