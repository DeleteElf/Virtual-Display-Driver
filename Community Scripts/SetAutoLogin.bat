@echo off
chcp 65001

REM 设置允许自动登录
reg add "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\PasswordLess\Device" /v DevicePasswordLessBuildVersion /t REG_DWORD /d "0" /f

echo 在打开的窗口中 取消勾选‌ “要使用本机，用户必须输入用户名和密码” 选项 ，并点击 "应用"。

echo 系统会弹出“自动登录”窗口，输入你当前账户的‌完整密码‌（如果是微软账户，请输入邮箱绑定的密码，而非PIN码），然后点击 ‌“确定”‌。

echo 正在准备进入设置界面...

pause

netplwiz

pause