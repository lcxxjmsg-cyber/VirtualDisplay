# VirtualDisplay

基于 Windows Indirect Display Driver (IddCx) 的虚拟显示器驱动与管理系统。

在真实物理屏幕之外创建多个虚拟显示器，支持自定义分辨率、刷新率、桌面布局拖拽、设为主屏、开机自恢复等功能。适用于多屏办公、远程串流（Sunshine / Moonlight）、虚拟桌面等场景。

## 功能特性

- **多虚拟显示器**：同时创建最多 4 个虚拟显示器（1080p / 2K / 4K / 5K / 8K 等任意分辨率）
- **自定义分辨率与刷新率**：不受固定档位限制，由系统显卡能力决定实际可用性（实测 144Hz 可用）
- **可视化布局拖拽**：类似 Windows 显示设置的布局面板，物理显示器为固定核心，虚拟显示器可拖拽吸附排版
- **设为主屏**：一键把虚拟显示器设为主屏（新窗口默认在其打开），物理屏变扩展屏；可一键恢复物理主屏
- **跟随物理屏**：快捷创建与物理主屏相同规格的虚拟显示器
- **HDR 支持**：驱动声明 IddCx 1.10 HDR 能力（实际可用性取决于 GPU 硬件）
- **渲染 GPU 选择**：为虚拟显示器指定渲染 GPU（自动 / 指定显卡）
- **开机自恢复**：登录时自动恢复上次的显示器配置
- **托盘**：托盘菜单快捷添加 / 关闭最小化到托盘
- **安装包**：MSI 安装（含驱动注册、桌面快捷方式、开始菜单、卸载）

## 系统要求

- Windows 10 19041+ / Windows 11（64 位）
- IddCx 1.4 – 1.11 运行时（系统自带）
- WebView2 Runtime（Edge 自带）

## 安装

1. 从 [Releases](https://github.com/lcxxjmsg-cyber/VirtualDisplay/releases) 下载 `VirtualDisplay.msi`
2. 双击安装（需要管理员权限）
3. 安装完成后从开始菜单或桌面快捷方式启动 **VirtualDisplay**
4. 若驱动签名证书未被信任，安装程序会自动安装测试签名证书

> 驱动为测试签名（VirtualDisplay Test Signing），无需开启 testsigning 模式；首次安装时若系统提示签名警告，选择"仍要安装"。

## 使用

### 添加显示器

- 首页点击快捷按钮（1080@60、2K@120、4K@60 等）一键添加
- 「跟随物理屏」：创建与物理主屏同规格的虚拟显示器
- 「+ 自定义…」：输入任意分辨率与刷新率（如 `2560 1440 144`）

### 布局拖拽

显示器页下方为布局面板：

- **黄色方块** = 物理显示器（固定核心，不可拖动）
- **蓝色方块** = 虚拟显示器（可拖动，拖动时自动吸附边缘对齐）
- 点击虚拟方块上的「设为主屏」或物理方块上的「恢复物理主屏」切换主屏
- 主屏显示器无法直接拖动（Windows 系统限制），请先恢复物理主屏再拖

### 设为虚拟屏为主屏

> 注意：设为虚拟屏为主屏后，新打开的程序窗口将默认显示在虚拟显示器上，物理显示器变成扩展屏。若你在物理屏前操作，程序窗口可能"看不见"。建议仅在串流等场景使用，操作后可随时恢复物理主屏。

### 开机自恢复

「自恢复」页 → 保存当前配置 → 打开「开机自恢复」开关（注册登录任务），下次登录时自动恢复。

## 命令行工具

安装目录下附带 `iddctrl.exe`：

```
iddctrl add <width> <height> [vsync]        添加显示器（vsync 单位 mHz）
iddctrl remove <index>                      移除显示器
iddctrl list --json                         列出显示器（真实编号）
iddctrl primary <index>                     设为主屏
iddctrl physical-primary                    恢复物理主屏
iddctrl layout <index:x,y> [...]            设置显示器位置
iddctrl advancedcolor [status|on|off] [index]  HDR 控制（按显示器）
iddctrl install --trust-certs               安装驱动
iddctrl uninstall                           卸载驱动
iddctrl save-config / restore               保存 / 恢复配置
iddctrl register-task [off]                 注册 / 移除开机自恢复
```

## 从源码构建

需要：Visual Studio 2022 (MSVC x64)、Windows 11 SDK (10.0.26100)、WDK (10.0.26100)、WebView2 SDK。

```bat
build.bat
```

产物输出到 `build\bin\`。重新签名驱动包（修改驱动后必须）：

```powershell
powershell -File signing\sign_cat.ps1
```

构建 MSI（需要 WiX Toolset 3.14）：

```bat
packaging\wix\candle.exe -arch x64 packaging\VirtualDisplay.wxs
packaging\wix\light.exe -ext WixUIExtension packaging\VirtualDisplay.wixobj
```

## 项目结构

```
driver\         IDD 驱动（UMDF + IddCx）
iddctrl\        命令行控制工具
gui\            WebView2 图形界面（HTML/CSS/JS）
signing\        驱动签名脚本
packaging\      WiX 安装包工程
```

## License

MIT
