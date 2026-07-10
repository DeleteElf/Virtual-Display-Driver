### 开发参考
https://learn.microsoft.com/zh-cn/windows-hardware/drivers/gettingstarted/provision-a-target-computer
大致的安装步骤如下：
1. 安装 C:\Program Files (x86)\Windows Kits\10\Remote\x64\WDK Test Target Setup x64-x64_en-us.msi
2. 如果目标计算机正在运行 Windows Server，请查找刚刚由 WDK 测试目标安装程序 MSI 创建的 DriverTest 文件夹。 （示例：c：\DriverTest）。 
   选择并按住 DriverTest 文件夹（或右键单击），然后选择 “属性”。 在“安全”选项卡上，向“经过身份验证的用户组”授予“修改”权限。

### 已知问题
1. 如果windows启用网络共享（非高级共享），会导致目录无法访问，引发驱动加载失败，这个问题暂时只能通过不启用网络共享来解决。

### 特别说明
1. windows 10 上内置的UMDF是1.4版本的，windows 11以上才内置2.0以上版本。