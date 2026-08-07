// taskbar_window.h — 任务栏窗口（Win11 视觉嵌入 + 双缓冲绘制）
//
// 重要说明（Win11）：
//   Win11 任务栏用 XAML/WinUI 重写，不再像 Win10 那样暴露 ReBarWindow32
//   容器供子窗口真正插入。SetParent hack 在 Win11 上会闪烁/被裁剪。
//   因此本模块采用与 TrafficMonitor(Win11) 相同的做法：创建一个
//   无边框、置顶、不抢焦点的分层窗口，精准贴附到任务栏空白区域，
//   视觉上"嵌"入任务栏。这是 Win11 上唯一稳定的方案。
#pragma once
#include <windows.h>
#include <string>
#include "config.h"
#include "monitor.h"

namespace mm {

// 任务栏显示窗口把右键设置请求转交给 App 的调度窗口处理。
inline constexpr UINT WM_MINIMONITOR_CONTEXT_MENU = WM_APP + 2;

class TaskbarWindow {
public:
    TaskbarWindow() = default;
    ~TaskbarWindow();

    // 创建窗口。hwnd_parent 留 nullptr（独立 topmost 窗口）。
    bool Create(HINSTANCE hinst, const Config& cfg);

    // 销毁。
    void Destroy();

    // 刷新一次显示。由 App 的定时器每秒调用。
    // 内部会重新采样并触发重绘。
    void Refresh(Monitor& monitor);

    // 重新定位（分辨率/任务栏变化、配置改动时调用）。
    void Reposition();

    // 应用新配置（颜色/字体/标签等变化时）。
    void ApplyConfig(const Config& cfg);

    // 设置右键菜单请求的接收窗口（通常是 App 的不可见调度窗口）。
    void SetContextMenuTarget(HWND target) { context_menu_target_ = target; }

    bool ContainsScreenPoint(POINT pt) const {
        RECT rc{};
        return hwnd_ && GetWindowRect(hwnd_, &rc) && PtInRect(&rc, pt);
    }

    HWND Hwnd() const { return hwnd_; }

    // 最近一次采样的指标缓存（供托盘提示等复用，避免重复采样）。
    const Metrics& LastMetrics() const { return last_metrics_; }
    bool HasMetrics() const { return has_metrics_; }

    // 注册窗口类用的类名。
    static const wchar_t* ClassName() { return L"MiniMonitorTaskbarWnd"; }

private:
    // 窗口过程（静态，转发到实例）
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    // 消息处理
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void    OnPaint();
    void    OnTimer();
    void    OnDestroy();

    // —— 绘制 ——
    // 在传入的内存 DC 上画完整内容（双缓冲）。返回绘制内容的总宽高。
    SIZE  DrawContent(HDC mem_dc, const Metrics& m);

    // 计算任务栏窗口应在的位置（Win11 定位算法）。
    // 任务栏在屏幕底部时：x 在右下角（避开托盘），y 垂直居中。
    RECT  CalcTaskbarRect();
    RECT  CalcDesiredRect(int content_w, int content_h);

    // 创建/销毁字体
    void RecreateFont();

    // 读取 Win11 深浅色主题（注册表），返回 true=深色
    bool IsDarkTheme();

    HINSTANCE   inst_     = nullptr;
    HWND        hwnd_     = nullptr;
    HWND        context_menu_target_ = nullptr;
    Config      cfg_;
    HFONT       font_     = nullptr;
    Monitor*    monitor_  = nullptr;   // 不持有，仅 Refresh 时借用

    // 上一次绘制的指标缓存（OnPaint 时复用，避免重复采样）
    Metrics     last_metrics_;
    bool        has_metrics_ = false;
};

} // namespace mm
