// app.cpp — 应用主类实现
#include "app.h"
#include "resource.h"
#include <shellapi.h>
#include <sstream>
#include <windowsx.h>

namespace mm {

// 调度窗口类名（不可见，仅做消息中枢）
static const wchar_t* kHiddenWndClass = L"MiniMonitorHiddenWnd";
// 开机自启注册表项
static const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValueName = L"MiniMonitor";

// —— TaskbarCreated 广播：Explorer 重启后任务栏窗口被销毁，需重建 ——
static UINT WM_TASKBARCREATED = 0;
App* App::active_instance_ = nullptr;

int App::Run(HINSTANCE hinst, int cmd_show) {
    inst_ = hinst;
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    if (!Init(hinst, cmd_show)) {
        // Init 可能已经创建了隐藏窗口或任务栏窗口，失败路径也要释放它们。
        Shutdown();
        return 1;
    }
    int ret = MessageLoop();
    Shutdown();
    return ret;
}

// ---------- 初始化 ----------
bool App::Init(HINSTANCE hinst, int cmd_show) {
    UNREFERENCED_PARAMETER(cmd_show);
    // 1. 配置
    cfg_mgr_.Init();
    cfg_ = cfg_mgr_.Load();
    monitor_.SetNetworkSelection(cfg_.network_interface);

    // 2. 调度窗口（必须先建，定时器和托盘回调都挂它）
    if (!CreateHiddenWindow()) return false;

    // 3. 任务栏窗口
    if (!taskbar_wnd_.Create(hinst, cfg_)) return false;
    taskbar_wnd_.UpdateFullscreenVisibility();
    // 透明显示层本身仍然鼠标穿透；低级回调只负责抑制监控区域的右键，
    // 避免同一个右键继续落到 Explorer 的任务栏菜单。
    taskbar_wnd_.SetContextMenuTarget(hidden_hwnd_);
    active_instance_ = this;
    right_click_hook_ = SetWindowsHookExW(WH_MOUSE_LL,
                                          &App::LowLevelMouseProc,
                                          inst_, 0);
    raw_mouse_registered_ = RegisterRawMouseInput();
    taskbar_wnd_.SetInputFallbackEnabled(!raw_mouse_registered_ &&
                                         !right_click_hook_);

    // 4. 托盘图标
    if (cfg_.show_tray_icon) {
        HICON icon = LoadAppIcon();
        tray_.Add(hinst, hidden_hwnd_, WM_TRAYICON, icon, L"MiniMonitor");
        UpdateTrayTip();
    }

    // 5. 采样定时器。注意首次会建立基线，第一次显示数值在 1 秒后。
    timer_id_ = SetTimer(hidden_hwnd_, 1, cfg_.sample_interval_ms, nullptr);
    if (!timer_id_) return false;
    // 立即先采样一次，建立基线（不刷新显示，避免全 0）
    monitor_.Update();

    // 开机自启状态同步（确保配置与注册表一致）
    cfg_.run_on_startup = IsRunOnStartup();

    return true;
}

// 不可见的调度窗口：接收 WM_TIMER / 托盘回调 / TaskbarCreated。
bool App::CreateHiddenWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &App::HiddenWndProc;
    wc.hInstance     = inst_;
    wc.lpszClassName = kHiddenWndClass;
    RegisterClassExW(&wc);

    // 这是内部消息窗口，不应出现在任务栏或 Alt+Tab 中。
    // 右键菜单弹出前，TrayIcon 会临时把它显示成 1x1 的屏幕外窗口，
    // 这样它可以合法地成为菜单的前台所有者，同时不会在桌面上闪出大窗口。
    hidden_hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, kHiddenWndClass, L"MiniMonitor", WS_POPUP,
        -32000, -32000, 1, 1,
        nullptr, nullptr, inst_, this);
    return hidden_hwnd_ != nullptr;
}

LRESULT CALLBACK App::HiddenWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lp);
        self = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        // TaskbarCreated 是动态注册的消息，无法进 switch，单独处理
        if (msg == WM_TASKBARCREATED && msg != 0) {
            // Explorer 重启 → 任务栏窗口可能失效，重建之
            self->taskbar_wnd_.Destroy();
            if (self->taskbar_wnd_.Create(self->inst_, self->cfg_)) {
                self->taskbar_wnd_.UpdateFullscreenVisibility();
                self->taskbar_wnd_.SetContextMenuTarget(self->hidden_hwnd_);
                self->taskbar_wnd_.SetInputFallbackEnabled(
                    !self->raw_mouse_registered_ &&
                    !self->right_click_hook_);
            }
            // Explorer 重启后通知区域图标通常也会被清空，需要重新添加。
            if (self->cfg_.show_tray_icon) {
                self->tray_.Remove();
                self->tray_.Add(self->inst_, self->hidden_hwnd_, WM_TRAYICON,
                                self->LoadAppIcon(), L"MiniMonitor");
                self->UpdateTrayTip();
            }
            return 0;
        }
        if (msg == WM_MINIMONITOR_CONTEXT_MENU) {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            self->ShowContextMenu(pt);
            return 0;
        }
        switch (msg) {
            case WM_TIMER:
                self->DoSample();
                return 0;
            case WM_INPUT:
                self->HandleRawMouseInput(reinterpret_cast<HRAWINPUT>(lp));
                return 0;
            case WM_ACTIVATEAPP:
                // 菜单打开期间如果其它应用被激活，主动结束当前菜单。
                // 这为系统菜单循环补上失焦兜底，避免 owner 窗口不可见时
                // 点击其它窗口后菜单仍残留在桌面上。
                if (!wp) EndMenu();
                return 0;
            case WM_CANCELMODE:
                EndMenu();
                return 0;
            case WM_COMMAND:
                {
                    const std::vector<NetworkInterfaceInfo> no_interfaces;
                    self->HandleCommand(static_cast<UINT>(LOWORD(wp)),
                                        no_interfaces);
                }
                return 0;
            case WM_TRAYICON:
                self->HandleTrayCallback(wp, lp);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------- 消息循环 ----------
int App::MessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 我们没有可见的对话框/加速键，直接 Translate/Dispatch 即可
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void App::Shutdown() {
    if (right_click_hook_) {
        UnhookWindowsHookEx(right_click_hook_);
        right_click_hook_ = nullptr;
    }
    right_click_captured_ = false;
    if (active_instance_ == this) active_instance_ = nullptr;
    UnregisterRawMouseInput();
    if (timer_id_) { KillTimer(hidden_hwnd_, timer_id_); timer_id_ = 0; }
    tray_.Remove();
    taskbar_wnd_.Destroy();
    if (hidden_hwnd_) { DestroyWindow(hidden_hwnd_); hidden_hwnd_ = nullptr; }
    // 退出前保存可持久化配置。
    cfg_.run_on_startup = IsRunOnStartup();
    cfg_mgr_.Save(cfg_);
}

bool App::RegisterRawMouseInput() {
    if (!hidden_hwnd_) return false;

    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;  // Generic Desktop Controls
    device.usUsage     = 0x02;  // Mouse
    // INPUTSINK 仅把一份 Raw Input 通知投递给本程序；不使用 NOLEGACY，
    // 因此 Explorer 和其它应用仍会照常收到全部标准鼠标消息。
    device.dwFlags     = RIDEV_INPUTSINK;
    device.hwndTarget  = hidden_hwnd_;
    return RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
}

void App::UnregisterRawMouseInput() {
    if (!raw_mouse_registered_) return;

    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage     = 0x02;
    device.dwFlags     = RIDEV_REMOVE;
    device.hwndTarget  = nullptr;
    RegisterRawInputDevices(&device, 1, sizeof(device));
    raw_mouse_registered_ = false;
}

void App::HandleRawMouseInput(HRAWINPUT input) {
    if (!raw_mouse_registered_ || !input || !hidden_hwnd_) return;

    RAWINPUT raw{};
    UINT size = sizeof(raw);
    const UINT bytes = GetRawInputData(input, RID_INPUT, &raw, &size,
                                       sizeof(RAWINPUTHEADER));
    if (bytes == static_cast<UINT>(-1) || raw.header.dwType != RIM_TYPEMOUSE) {
        return;
    }
    POINT pt{};
    const USHORT buttons = raw.data.mouse.usButtonFlags;
    const USHORT click_buttons = RI_MOUSE_LEFT_BUTTON_DOWN
                               | RI_MOUSE_LEFT_BUTTON_UP
                               | RI_MOUSE_RIGHT_BUTTON_DOWN
                               | RI_MOUSE_RIGHT_BUTTON_UP;
    if ((buttons & click_buttons) != 0 && GetCursorPos(&pt)) {
        // TrackPopupMenu 的系统模态循环在某些前台激活场景下不会把
        // 外部点击完整转成取消消息；这里仅观察 Raw Input，在菜单外
        // 主动结束菜单。WindowFromPoint 命中菜单本身时保持正常选项点击。
        if (tray_.IsMenuOpen() && !tray_.IsPointInMenu(pt)) {
            tray_.CancelMenu();
            return;
        }
    }

    if (!right_click_hook_ &&
        (buttons & RI_MOUSE_RIGHT_BUTTON_UP) != 0 &&
        GetCursorPos(&pt) && taskbar_wnd_.ContainsScreenPoint(pt)) {
        // 仅在低级回调不可用的兼容回退路径中，观察标准右键抬起后弹菜单；
        // 正常路径由 LowLevelMouseProc 负责抑制系统右键并投递请求。
        PostMessageW(hidden_hwnd_, WM_MINIMONITOR_CONTEXT_MENU,
                     0, MAKELPARAM(pt.x, pt.y));
    }
}

LRESULT CALLBACK App::LowLevelMouseProc(int code, WPARAM wp, LPARAM lp) {
    App* self = active_instance_;
    if (code == HC_ACTION && self && lp) {
        const bool right_down = wp == WM_RBUTTONDOWN || wp == WM_NCRBUTTONDOWN;
        const bool right_up   = wp == WM_RBUTTONUP   || wp == WM_NCRBUTTONUP;
        if (right_down || right_up) {
            const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            const POINT pt = mouse->pt;

            if (right_down) {
                // 每次新的右键按下都从干净状态开始，避免丢失上一轮抬起
                // 后把下一次外部右键误认为仍属于监控区域。
                self->right_click_captured_ = false;
                if (self->taskbar_wnd_.ContainsScreenPoint(pt)) {
                    self->right_click_captured_ = true;
                    // 只吞掉监控矩形内的右键按下，防止 Explorer 开始自己的菜单。
                    return 1;
                }
            } else if (right_up && self->right_click_captured_) {
                self->right_click_captured_ = false;
                if (self->hidden_hwnd_ &&
                    self->taskbar_wnd_.ContainsScreenPoint(pt)) {
                    PostMessageW(self->hidden_hwnd_,
                                 WM_MINIMONITOR_CONTEXT_MENU,
                                 0, MAKELPARAM(pt.x, pt.y));
                }
                // 配对吞掉右键抬起，Explorer 不会再收到不完整的右键序列。
                return 1;
            }
        }
    }

    // 所有非目标事件，以及监控区域外的右键，必须继续传给下一个钩子。
    return CallNextHookEx(nullptr, code, wp, lp);
}

// ---------- 业务 ----------
void App::DoSample() {
    // 先处理前台全屏状态，再采样 + 刷新任务栏窗口。
    // UpdateFullscreenVisibility 的判断很轻，不需要额外高频定时器。
    taskbar_wnd_.UpdateFullscreenVisibility();
    taskbar_wnd_.Refresh(monitor_);
    UpdateTrayTip();
}

void App::HandleTrayCallback(WPARAM /*wp*/, LPARAM lp) {
    // wp 是图标 ID，lp 低字是鼠标消息
    UINT mouse = LOWORD(lp);
    switch (mouse) {
        case WM_LBUTTONDBLCLK:
            // 不再把双击托盘图标解释为隐藏窗口；设置统一从右键菜单进入。
            break;
        case WM_LBUTTONUP:
            // 左键单击不执行操作。
            break;
        case WM_RBUTTONUP: {
            POINT pt{};
            if (GetCursorPos(&pt)) ShowContextMenu(pt);
            break;
        }
        case WM_MOUSEMOVE:
            // 鼠标悬停时不做事，提示文字已由 SetTip 维护
            break;
    }
}

void App::HandleCommand(UINT cmd,
                        const std::vector<NetworkInterfaceInfo>& interfaces) {
    if (cmd >= IDM_NETWORK_INTERFACE_BASE) {
        const size_t index = static_cast<size_t>(cmd - IDM_NETWORK_INTERFACE_BASE);
        if (index < interfaces.size() && index < 100) {
            SetNetworkSelection(interfaces[index].id);
            return;
        }
    }

    switch (cmd) {
        case IDM_RUN_STARTUP:  ToggleStartup(); break;
        case IDM_TOGGLE_TRAY:  ToggleTrayIcon(); break;
        case IDM_NETWORK_AUTO: SetNetworkSelection(kNetworkSelectionAuto); break;
        case IDM_NETWORK_ALL:  SetNetworkSelection(kNetworkSelectionAll); break;
        case IDM_OPEN_TASKMGR: OpenTaskManager(); break;
        case IDM_ABOUT:        ShowAbout(); break;
        case IDM_EXIT:         Exit(); break;
    }
}

void App::ShowContextMenu(POINT pt) {
    const auto interfaces = monitor_.EnumerateNetworkInterfaces();
    UINT cmd = tray_.PopupMenu(hidden_hwnd_, pt, cfg_.run_on_startup,
                               cfg_.show_tray_icon, cfg_.network_interface,
                               interfaces);
    if (cmd != 0) HandleCommand(cmd, interfaces);
}

void App::ToggleStartup() {
    cfg_.run_on_startup = !cfg_.run_on_startup;
    SetRunOnStartup(cfg_.run_on_startup);
    cfg_mgr_.Save(cfg_);
}

void App::ToggleTrayIcon() {
    cfg_.show_tray_icon = !cfg_.show_tray_icon;
    if (cfg_.show_tray_icon) {
        tray_.Add(inst_, hidden_hwnd_, WM_TRAYICON, LoadAppIcon(), L"MiniMonitor");
        UpdateTrayTip();
    } else {
        tray_.Remove();
    }
    cfg_mgr_.Save(cfg_);
}

void App::SetNetworkSelection(const std::wstring& selection) {
    cfg_.network_interface = selection.empty()
        ? std::wstring(kNetworkSelectionAuto) : selection;
    monitor_.SetNetworkSelection(cfg_.network_interface);
    cfg_mgr_.Save(cfg_);
    // 立即刷新，让用户切换后不用等下一次定时器。
    taskbar_wnd_.Refresh(monitor_);
    UpdateTrayTip();
}

void App::OpenTaskManager() {
    // 用 ShellExecute 启动 taskmgr，对齐你 config.ini 里的 double_click_exe
    ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
}

void App::ShowAbout() {
    // 简单的关于框，用 MessageBox 避免资源文件依赖
    MessageBoxW(hidden_hwnd_,
        L"MiniMonitor v1.0\n\n"
        L"轻量任务栏监控器\n"
        L"实时显示 上传/下载/CPU/内存\n\n"
        L"技术：纯 Win32 API，静态链接 CRT\n"
        L"目标：低内存、低 CPU 占用（实际值取决于系统与构建）\n\n"
        L"参考：TrafficMonitor (zhongyang219/TrafficMonitor)",
        L"关于 MiniMonitor", MB_OK | MB_ICONINFORMATION);
}

void App::Exit() {
    // 销毁调度窗口 → WM_DESTROY → PostQuitMessage → 消息循环退出
    if (hidden_hwnd_) DestroyWindow(hidden_hwnd_);
}

// ---------- 开机自启 ----------
bool App::IsRunOnStartup() {
    HKEY hkey = nullptr;
    bool found = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH] = {};
        DWORD size = sizeof(buf);
        if (RegQueryValueExW(hkey, kRunValueName, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
            found = true;
        }
        RegCloseKey(hkey);
    }
    return found;
}

void App::SetRunOnStartup(bool enable) {
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hkey) != ERROR_SUCCESS)
        return;
    if (enable) {
        // 用当前 exe 全路径，带 -silent 参数（虽然现在不解析，预留）
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring val = std::wstring(L"\"") + exe + L"\"";
        RegSetValueExW(hkey, kRunValueName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(val.c_str()),
                       static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hkey, kRunValueName);
    }
    RegCloseKey(hkey);
}

// ---------- 工具 ----------
HICON App::LoadAppIcon() {
    // 优先从资源加载（app.rc 里的 IDI_APP_ICON），失败则用系统默认。
    HICON icon = static_cast<HICON>(
        LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP_ICON),
                   IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    if (!icon) {
        icon = LoadIcon(nullptr, IDI_APPLICATION);
    }
    return icon;
}

void App::UpdateTrayTip() {
    if (!tray_.IsAdded()) return;
    // 复用 TaskbarWindow 的最近一次采样缓存，绝不重复采样（否则会推进
    // monitor 的差分基线，导致下一次速率失真）。
    if (!taskbar_wnd_.HasMetrics()) return;
    const Metrics& m = taskbar_wnd_.LastMetrics();

    std::wostringstream oss;
    oss << L"MiniMonitor\n"
        << L"↑ " << FormatSpeed(m.net_up_bytes_per_sec, true)
        << L"  ↓ " << FormatSpeed(m.net_down_bytes_per_sec, true)
        << L"\nCPU " << m.cpu_usage << L"%"
        << L"  内存 " << m.memory_usage << L"%";
    tray_.SetTip(oss.str().c_str());
}

} // namespace mm
