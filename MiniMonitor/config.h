// config.h — INI 配置读写
// 用 Win32 的 GetPrivateProfile* / WritePrivateProfile* API，
// 零依赖、UTF-16 文件、可被记事本直接编辑。
#pragma once
#include <windows.h>
#include <string>

namespace mm {

struct Config {
    // —— 采集 ——
    int  sample_interval_ms = 1000;     // 采样间隔（毫秒），默认 1 秒

    // —— 显示 ——
    std::wstring font_name   = L"微软雅黑";
    int          font_size   = 9;       // pt
    bool         font_bold   = false;

    // 颜色（RGB，BGR 无关，这里直接存 RGB 由绘制端处理）
    COLORREF text_color   = RGB(255, 255, 255);   // 默认白字（任务栏深色底）
    COLORREF back_color   = RGB(0, 0, 0);         // 默认黑底（可被透明色吃掉）
    bool     transparent  = true;                // 透明背景
    bool     auto_theme   = true;                 // 自动跟随 Win11 深浅色主题

    // 文本标签（对齐 TrafficMonitor 的 up_string 等）
    std::wstring up_str     = L"↑: ";
    std::wstring down_str   = L"↓: ";
    std::wstring cpu_str    = L"CPU: ";
    std::wstring mem_str    = L"内存: ";

    bool short_speed_unit  = false;   // 速率省略单位
    bool hide_percent      = false;   // 百分比省略 %
    bool separate_unit_space = true;  // 数值与单位间留空格
    int  item_space        = 28;      // 两列之间的间距（像素），让布局更舒展

    // —— 任务栏定位 ——
    // 找不到 Explorer 托盘容器时的回退留白（像素）；正常情况下会以
    // TrayNotifyWnd 的实际左边界为硬上限，防止覆盖系统图标。
    int  taskbar_right_space = 180;
    int  window_offset_x    = 0;       // 横向微调
    int  window_offset_y    = 0;       // 纵向微调
    bool show_on_left       = false;   // 显示在任务栏左侧（开始菜单旁）

    // —— 行为 ——
    bool run_on_startup     = false;   // 开机自启
    bool show_tray_icon     = true;    // 显示托盘图标
};

// 从 exe 同目录下的 MiniMonitor.ini 读取配置。
// 文件不存在时返回默认值，并在首次 Save 时生成。
class ConfigManager {
public:
    // 初始化：确定 ini 文件全路径（exe 同目录）。
    void Init();

    // 读取（不存在的键用 Config 默认值）。
    Config Load();

    // 写回。传任意 Config，全量写盘。
    void Save(const Config& c);

    const std::wstring& FilePath() const { return path_; }

private:
    std::wstring path_;   // MiniMonitor.ini 全路径
};

} // namespace mm
