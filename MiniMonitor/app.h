// app.h — 应用主类，串起各模块
//
// 职责：持有 Monitor / TaskbarWindow / TrayIcon / Config，
//       运行消息循环，处理定时器与托盘回调。
#pragma once
#include <windows.h>
#include <shellapi.h>
#include "config.h"
#include "monitor.h"
#include "taskbar_window.h"
#include "tray_icon.h"

namespace mm {

class App {
public:
    // 单实例入口。返回值是 WinMain 的返回码。
    int Run(HINSTANCE hinst, int cmd_show);

private:
    // —— 初始化 ——
    bool Init(HINSTANCE hinst, int cmd_show);
    void Shutdown();

    // —— 消息循环 ——
    int  MessageLoop();

    // —— 隐藏的"调度窗口" ——
    // 我们需要一个窗口来接收定时器和托盘回调，但不能是任务栏窗口
    // （任务栏窗口有 WS_EX_NOACTIVATE，不适合做消息中枢）。
    // 所以单独建一个不可见的"调度窗口"。
    bool CreateHiddenWindow();
    static LRESULT CALLBACK HiddenWndProc(HWND, UINT, WPARAM, LPARAM);
    // 轻量级右键钩子：仅在 WM_RBUTTONUP 且坐标落在监控窗口矩形内时
    // 拦截并弹出设置菜单。其余所有鼠标事件立即 CallNextHookEx 放行，
    // 不做任何处理，开销极小。
    // 必要性：监控窗口用 LWA_COLORKEY 透明背景，Windows 规定颜色键
    // 区域鼠标穿透，导致右键点在文字间空白处穿到任务栏而收不到消息。
    static LRESULT CALLBACK LowLevelRightClickProc(int code, WPARAM wp, LPARAM lp);
    void HandleTrayCallback(WPARAM wp, LPARAM lp);
    void HandleCommand(UINT cmd);
    void ShowContextMenu(POINT pt);

    // —— 业务动作 ——
    void DoSample();              // 定时器触发：采样 + 刷新任务栏
    void ToggleStartup();         // 切换开机自启
    void ToggleTrayIcon();        // 切换是否显示托盘图标
    void OpenTaskManager();
    void ShowAbout();
    void Exit();

    // —— 开机自启（注册表 HKCU\...\Run）——
    bool IsRunOnStartup();
    void SetRunOnStartup(bool enable);

    // —— 工具 ——
    HICON LoadAppIcon();
    void  UpdateTrayTip();        // 用最新数值刷新托盘提示

    HINSTANCE        inst_        = nullptr;
    HWND             hidden_hwnd_ = nullptr;   // 调度窗口（不可见）
    HHOOK            right_click_hook_ = nullptr;  // 仅拦截监控区右键
    UINT_PTR         timer_id_    = 0;          // 采样定时器

    ConfigManager    cfg_mgr_;
    Config           cfg_;
    Monitor          monitor_;
    TaskbarWindow    taskbar_wnd_;
    TrayIcon         tray_;

    // App 为单实例；静态钩子回调通过它访问任务栏窗口与调度窗口。
    static App* active_instance_;
};

} // namespace mm
