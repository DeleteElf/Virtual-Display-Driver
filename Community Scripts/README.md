#Scripts

这是一组在驱动开发过程中编写的PowerShell脚本集。欢迎提出改进意见或新增其他功能。我们并未始终专注于保持脚本的"时效性"，这意味着这些脚本所依赖的模块可能发生的变更也未得到修正。它们更多是作为一个框架，供"实验者"利用PowerShell即时修改设置。使用这些脚本需至少熟悉PowerShell并理解管理员权限的风险与回报。

以下是每段脚本的简短说明：

silent‑install.ps1 – 静默地从GitHub获取最新的已签名虚拟显示驱动程序，通过DevCon进行安装，然后清理临时工作区。
modules_install.bat – 打开一个提升的PowerShell会话，该会话安装DisplayConfig和MonitorConfig模块，以便每个帮助脚本都有其先决条件。
set‑dependencies.ps1 – 验证所需的确切模块版本，按需安装或导入它们，如果缺少任何内容，则中止下游执行。
get_disp_num.ps1 – 通过扫描WMI以查找自定义的“MTT1337”监视器标识符，返回VDD屏幕的数字适配器ID。
changeres‑VDD.ps1 – 将虚拟面板的分辨率热交换为您在命令行上传递的宽度和高度。
refreshrate‑VDD.ps1 – 根据安全列表验证值后，更改VDD监视器的刷新率（30-500Hz）。
rotate‑VDD.ps1 – 将虚拟显示器旋转90°，180°，或270°通过使用匹配的旋转标记调用DisplayConfig。
scale‑VDD.ps1 – 在虚拟监视器上设置或重置DPI缩放，遵守Windows允许的最大缩放因子。
HDRswitch‑VDD.ps1 – 一键即可在SDR（8位）和HDR（10位）颜色模式之间翻转虚拟屏幕。不确定它是否还能继续工作，因为moduels发生了变化）
    primary‑VDD.ps1 – 将虚拟显示器设置为Windows主显示器，以便您可以从无头设备流式传输或远程桌面。
toggle‑VDD.ps1 – 一键式PowerShell开关，首先将自身提升为管理员，然后启用或禁用您的虚拟显示驱动程序，并立即在扩展和克隆桌面之间翻转Windows，非常适合需要按需将虚拟显示器联机或脱机的流媒体用户。
winp‑VDD.ps1 – 一个轻量级的配套脚本，使驱动程序不受影响，只需在扩展和克隆模式之间切换窗口，在虚拟显示器应永久启用时为您提供即时的“演示切换”。
