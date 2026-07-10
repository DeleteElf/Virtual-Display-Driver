@echo off
chcp 65001

echo "正在安装虚拟显示器..."

REM 将我们的配置目录写入注册表
rem install
set "DRIVER_DIR=%~dp0VirtualDisplayDriver"
echo driver directory:%DRIVER_DIR%
rem Get root directory,current in the scripts directory.
for %%I in ("%~dp0\..\..") do set "ROOT_DIR=%%~fI"
set "CONFIG_DIR=%ROOT_DIR%\config"
set "VDD_CONFIG=%CONFIG_DIR%\option.txt"
echo driver config path: %VDD_CONFIG%
REM 总是从原始目录中拷贝配置到配置目录，不管是用于恢复还是备份，都是不错的选择
copy "%DRIVER_DIR%\option.txt" "%VDD_CONFIG%"
REM 因为默认读的是注册表配置的目录，我们需要将注册表的目录配置一下
reg add "HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver" /v path /t REG_SZ /d "%CONFIG_DIR%" /f
REM 注册表设置允许远程RPC
reg add "HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Terminal Server" /v AllowRemoteRPC /t REG_DWORD /d "1" /f

REM 安装具体的驱动
powershell.exe -ExecutionPolicy Bypass -File "%~dp0silent-install.ps1"

REM 初次安装后，还应设置成扩展这些显示器的显示模式
powershell -NoProfile -ExecutionPolicy Bypass -Command "& "C:\Windows\System32\DisplaySwitch.exe" /extend"

echo "虚拟显示器安装完成"