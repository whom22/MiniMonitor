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
    // Raw Input 用于观察菜单外点击；低级鼠标回调只在监控矩形内抑制右键，
    // 防止 Explorer 同时弹出任务栏原生菜单，其余鼠标事件全部继续传递。
    bool RegisterRawMouseInput();
    void UnregisterRawMouseInput();
    void HandleRawMouseInput(HRAWINPUT input);
    static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wp, LPARAM lp);
    void HandleTrayCallback(WPARAM wp, LPARAM lp);
    void HandleCommand(UINT cmd,
                       const std::vector<NetworkInterfaceInfo>& interfaces);
    void ShowContextMenu(POINT pt);

    // —— 业务动作 ——
    void DoSample();              // 定时器触发：采样 + 刷新任务栏
    void ToggleStartup();         // 切换开机自启
    void ToggleTrayIcon();        // 切换是否显示托盘图标
    void SetNetworkSelection(const std::wstring& selection);
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
    HHOOK            right_click_hook_ = nullptr;
    bool             right_click_captured_ = false;
    bool             raw_mouse_registered_ = false;
    UINT_PTR         timer_id_    = 0;          // 采样定时器

    ConfigManager    cfg_mgr_;
    Config           cfg_;
    Monitor          monitor_;
    TaskbarWindow    taskbar_wnd_;
    TrayIcon         tray_;

    // App 为单实例；低级鼠标回调只通过这个指针访问当前实例。
    static App* active_instance_;

};

} // namespace mm
