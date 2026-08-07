# MiniMonitor — 轻量任务栏监控器

仿照 [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 的思路，从零用 **纯 C++ Win32 API** 写的轻量监控程序。在任务栏实时显示 **上传/下载速率、CPU、内存** 四项指标。

> 目标：**内存和存储占用尽可能小，不影响电脑运行**。

---

## 特性

- ✅ 任务栏实时显示：`↑: 1.23 MB/s   ↓: 4.56 MB/s   CPU: 12%   内存: 45%`
- ✅ Win11 深色/浅色主题自动适配
- ✅ 单位智能转换（B/s → KB/s → MB/s → GB/s）
- ✅ 系统托盘图标 + 右键菜单（显示/隐藏、开机自启、任务管理器、关于、退出）
- ✅ 单实例运行（多开自动退出）
- ✅ 单 exe 文件，无第三方运行时 DLL 依赖（Windows 系统 DLL 除外）
- ✅ 分辨率/DPI 变化自动重新定位

## 占用（本机 Release 实测）

以下数据来自当前机器的 30 秒空闲运行测试，仅用于校准量级，不替代不同硬件、主题或显示缩放下的实测。

| 项目 | 数值 |
|------|------|
| Release exe 体积 | 1.015 MiB |
| 工作集 | 11.49–11.53 MiB |
| 私有字节 | 1.61–1.80 MiB |
| CPU 占用 | 两次 30 秒测试约 0.05–0.36% 单核等效；20 逻辑处理器总占用约 0.003–0.018% |
| GDI / USER 对象 | 9 / 6，30 秒内不增长 |
| 句柄 | 134，30 秒内不增长 |
| 依赖 | 仅 Windows 系统 DLL，无 MinGW/libwinpthread 等第三方运行时 DLL |

---

## 如何编译

### 方法 A：Visual Studio 2022（推荐）

1. **安装 Visual Studio 2022 Community**（免费）：
   - 下载：https://visualstudio.microsoft.com/zh-hans/vs/community/
   - 安装时勾选 **"使用 C++ 的桌面开发"** 工作负载（含 MSVC、Windows SDK）。

2. **打开工程**：
   - 双击 `MiniMonitor.sln`，或在 VS 里 `文件 → 打开 → 项目/解决方案` 选它。

3. **编译**：
   - 顶部选 `Release | x64`。
   - `生成 → 生成解决方案`（或 `Ctrl+Shift+B`）。
   - 产物：`build/Release/MiniMonitor.exe`

### 方法 B：CMake（命令行）

需先装 VS 2022（或仅 [Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)）+ CMake。

```bash
cmake -B build -S . -A x64
cmake --build build --config Release
# 产物：build/bin/Release/MiniMonitor.exe
```

### 方法 C：命令行（MSBuild，无需打开 IDE）

在「开始菜单」找 **"x64 Native Tools Command Prompt for VS 2022"**，运行：

```bash
msbuild MiniMonitor.sln /p:Configuration=Release /p:Platform=x64
```

---

## 使用

直接双击 `MiniMonitor.exe` 运行。首次正常退出时会在 exe 同目录生成 `MiniMonitor.ini` 配置文件（绿色便携）。

- **监控区域/托盘右键**：开机自启、托盘图标开关、打开任务管理器、关于、退出
- 监控文字始终显示；不再提供容易误触的“隐藏窗口”功能
- **鼠标悬停托盘图标**：显示当前数值

---

## 配置（MiniMonitor.ini）

用记事本即可编辑，改完保存后**重启程序**生效。关键项：

```ini
[General]
sample_interval_ms = 1000        ; 采样间隔（毫秒），越小越实时但越耗电

[Display]
font_name = 微软雅黑
font_size = 9
auto_theme = 1                   ; 1=跟随系统深浅色主题
text_color = 255,255,255         ; R,G,B（auto_theme=1 时此项被覆盖）
back_color = 0,0,0
up_str = "↑: "                   ; 各项前缀标签
down_str = "↓: "
cpu_str = "CPU: "
mem_str = "内存: "
short_speed_unit = 0             ; 1=速率不显示单位（更窄）
hide_percent = 0                 ; 1=百分比不显示 % 号
item_space = 20                  ; 两列之间的间距（像素）

[Taskbar]
taskbar_right_space = 180        ; 找不到托盘容器时的回退留白；正常情况自动读取实际托盘边界
window_offset_x = 0              ; 横向微调（正=往右）
window_offset_y = 0              ; 纵向微调（正=往下）
show_on_left = 0                 ; 1=显示在任务栏左侧（开始按钮旁）

[Behavior]
run_on_startup = 0               ; 开机自启（也可在托盘菜单切换）
show_tray_icon = 1               ; 显示托盘图标（可在监控区域右键关闭）
```

**位置不合适时怎么调？** 先看窗口大概偏哪边：
- 程序会优先读取 Explorer 的 `TrayNotifyWnd` 实际左边界，窗口不会覆盖托盘/时钟图标
- 如果系统没有暴露该托盘窗口，才使用 `taskbar_right_space` 作为回退值
- 太高/太低 → 调 `window_offset_y`

---

## 项目结构

```
MiniMonitor/
├── MiniMonitor.sln              # VS 解决方案
├── CMakeLists.txt               # CMake 构建
├── README.md
└── MiniMonitor/
    ├── MiniMonitor.vcxproj      # VS 工程
    ├── main.cpp                 # WinMain：单实例 + DPI 感知
    ├── app.h / app.cpp          # 应用主类，串起各模块 + 消息循环
    ├── monitor.h / monitor.cpp  # 数据采集：流量/CPU/内存（核心）
    ├── taskbar_window.h/.cpp    # 任务栏窗口：Win11 定位 + 双缓冲绘制
    ├── tray_icon.h / .cpp       # 托盘图标 + 右键菜单
    ├── config.h / config.cpp    # INI 配置读写
    ├── resource.h / app.rc      # 资源（图标、清单）
    ├── app.manifest             # DPI 感知 + 兼容性声明
    └── app.ico                  # 应用图标（占位，可替换）
```

---

## 实现要点（为什么这样设计能"占用最小"）

### 1. 数据采集 —— 纯系统 API，零依赖
- **网络流量**：`GetIfTable2`（IP Helper API）枚举网卡，累加所有活动网卡的 `InOctets/OutOctets`，两次采样差分得速率。**不依赖任何第三方库**（TrafficMonitor 也是这个做法）。
- **CPU**：`GetSystemTimes` 拿 idle/kernel/user 三个时间，差分算占用率。比性能计数器轻得多。
- **内存**：`GlobalMemoryStatusEx` 的 `dwMemoryLoad` 直接给百分比。

### 2. 任务栏窗口 —— Win11 视觉嵌入
> ⚠️ **重要事实**：Windows 11 把任务栏用 XAML/WinUI 完全重写，**不再支持** Win10 那种把子窗口真正 `SetParent` 插入任务栏窗口树的做法。你的系统（Win11 build 26200）也是如此。

本程序（以及 TrafficMonitor 在 Win11 上）采用**视觉嵌入**：
- 创建一个 `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED` 的无边框窗口
- 精准贴附到任务栏空白区域，看起来像嵌进去了一样
- `WS_EX_NOACTIVATE` 避免窗口激活；任务栏跨 Explorer 进程的点击穿透仍以目标系统实测为准

### 3. 绘制 —— 双缓冲防闪烁
所有内容先画到内存位图，再 `BitBlt` 到屏幕，杜绝重绘闪烁（长跑必须）。

### 4. 性能优化
- 静态链接 CRT（`/MT`）→ 单 exe，无 vcruntime DLL 依赖
- 不开工作线程，全在主消息循环跑（1 秒一次的采样完全不需要线程）
- GDI 对象严格 `DeleteObject` / `DeleteDC` 释放，无泄漏
- 单实例互斥锁，避免多开重复占用

---

## 与 TrafficMonitor 的关系

- 本目录原本只有 **TrafficMonitor 的编译后程序**（无源码）。
- 本项目是**从零参考其思路**重写的简化版，砍掉了：
  - 硬件温度监控（需 `LibreHardwareMonitorLib.dll`，体积大、需管理员）
  - GPU、硬盘、主板等扩展项
  - 皮肤系统、插件系统、历史流量统计
  - MFC 框架（改用纯 Win32，进一步缩体积）
- 保留并聚焦在**你最需要的核心 4 项**：上传、下载、CPU、内存。

如需扩展（加 GPU/温度等），可参考 TrafficMonitor 源码：https://github.com/zhongyang219/TrafficMonitor

---

## 已知限制 / 后续可扩展

- ⚠️ 指标窗口返回 `HTTRANSPARENT` 尝试点击穿透；Windows 官方语义明确保证的是同线程窗口，任务栏属于 Explorer 进程，跨进程效果仍需在目标系统实机确认
- [ ] 鼠标悬停 tooltip 显示完整数值
- [ ] 配置 GUI（目前靠编辑 ini）
- [ ] 历史流量曲线图
- [ ] 网速突增提醒

---

## 许可

参考 TrafficMonitor（GPLv3）思路自研。可自由使用、修改。
