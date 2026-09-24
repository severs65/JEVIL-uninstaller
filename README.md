# JEVIL Uninstaller

一个轻量的 Windows 软件卸载与残留清理工具。使用 C++ / Win32 原生编写，编译为单个可执行文件，**无需安装任何运行环境，双击即可使用**。界面简洁，中文显示。

## 功能

- **程序列表**：枚举系统中已安装的软件并显示名称、大小、安装日期与图标；同时识别没有标准注册表卸载项的程序、用户级安装程序和 UWP（微软商店）应用。
- **正常卸载**：先调用软件自带的卸载程序，完成后扫描残留，由用户**逐项勾选**要清理的文件、文件夹和注册表项。
- **强制移除**：当软件自带卸载不可用或文件被占用时，可停止相关进程与服务、重置文件权限后移除；仍无法删除的项目登记为系统重启后自动删除。
- **安装监控**（可选）：记录安装过程产生的变化，便于日后还原。

## 安全设计

本工具会修改系统文件与注册表，因此默认采取保守、可恢复的策略：

- **受保护位置不删除**：系统关键服务与驱动、Windows 目录、Program Files 与 ProgramData 等顶层目录、用户配置关键目录一律受保护，不会被批量移除。
- **删除前确认**：所有残留项以列表展示并默认由用户勾选，不做无提示删除。
- **注册表备份**：删除注册表项前自动导出备份到 `C:\ProgramData\JEVILRegBackup`，需要时可手动还原。
- **不常驻、不联网**：程序不设置开机自启动，不收集任何数据，不发起网络请求。

## 系统要求

- Windows 10 / Windows 11（64 位）。
- 部分操作需要管理员权限，程序会通过 UAC 主动请求。

## 从源码构建

需安装 Visual Studio（含 C++ 桌面开发组件，MSVC + Windows SDK）。

在 `src` 目录下执行：

```bat
build.bat
```

或手动编译（在 “x64 Native Tools Command Prompt” 中）：

```bat
rc /nologo resource.rc
cl /nologo /O2 /MT /utf-8 /DUNICODE /D_UNICODE main.cpp resource.res /link /SUBSYSTEM:WINDOWS /OUT:JEVILUninstaller.exe
```

`/MT` 为静态链接，生成的 exe 不依赖 VC++ 运行库，可在其他电脑直接运行。

## 发布与签名

正式发布的可执行文件**仅由本公开仓库的源代码经 GitHub Actions 构建产生**，构建脚本见 `.github/workflows/build.yml`，保证产物可复现、可审计。

## 许可证

[MIT License](LICENSE)，为 OSI 认可的开源许可证。
