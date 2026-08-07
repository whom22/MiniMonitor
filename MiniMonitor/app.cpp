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

    // 2. 调度窗口（必须先建，定时器和托盘回调都挂它）
    if (!CreateHiddenWindow()) return false;

    // 3. 任务栏窗口
    if (!taskbar_wnd_.Create(hinst, cfg_)) return false;
    // 右键菜单请求由任务栏窗口通过 WM_MINIMONITOR_CONTEXT_MENU 转发到这里。
    taskbar_wnd_.SetContextMenuTarget(hidden_hwnd_);
    // 先登记实例指针，再安装钩子，确保钩子回调能安全访问实例。
    active_instance_ = this;
    // 安装轻量右键钩子：监控窗口用颜色键透明，背景区鼠标会穿透到任务栏，
    // 导致右键点空白处收不到。钩子仅在右键抬起且坐标在监控窗口内时拦截。
    right_click_hook_ = SetWindowsHookExW(WH_MOUSE_LL,
                                          &App::LowLevelRightClickProc,
                                          inst_, 0);

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

    hidden_hwnd_ = CreateWindowExW(
        0, kHiddenWndClass, L"MiniMonitor", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
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
                self->taskbar_wnd_.SetContextMenuTarget(self->hidden_hwnd_);
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
            case WM_COMMAND:
                self->HandleCommand(static_cast<UINT>(LOWORD(wp)));
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
    if (active_instance_ == this) active_instance_ = nullptr;
    if (timer_id_) { KillTimer(hidden_hwnd_, timer_id_); timer_id_ = 0; }
    tray_.Remove();
    taskbar_wnd_.Destroy();
    if (hidden_hwnd_) { DestroyWindow(hidden_hwnd_); hidden_hwnd_ = nullptr; }
    // 退出前保存可持久化配置。
    cfg_.run_on_startup = IsRunOnStartup();
    cfg_mgr_.Save(cfg_);
}

// 轻量右键钩子。设计目标：对系统鼠标的影响降到最小。
// - 只在 HC_ACTION + WM_RBUTTONUP 时做一次 PtInRect，其它情况零开销放行。
// - 不调用任何阻塞 API；用 PostMessage 异步把菜单请求投给调度窗口。
// - 不持有锁、不访问易变状态，仅读取窗口矩形。
LRESULT CALLBACK App::LowLevelRightClickProc(int code, WPARAM wp, LPARAM lp) {
    App* self = active_instance_;
    // 绝大多数回调：code != HC_ACTION、非右键抬起、或实例未就绪，立即放行。
    if (code == HC_ACTION && wp == WM_RBUTTONUP && self) {
        const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (mouse) {
            POINT pt{ mouse->pt.x, mouse->pt.y };
            // 命中监控窗口才拦截；否则放行给任务栏/其它窗口。
            if (self->taskbar_wnd_.ContainsScreenPoint(pt) && self->hidden_hwnd_) {
                PostMessageW(self->hidden_hwnd_, WM_MINIMONITOR_CONTEXT_MENU,
                             0, MAKELPARAM(pt.x, pt.y));
                // 吞掉这次右键抬起，阻止 Explorer 在监控文字上弹系统菜单。
                return 1;
            }
        }
    }
    return CallNextHookEx(self ? self->right_click_hook_ : nullptr, code, wp, lp);
}

// ---------- 业务 ----------
void App::DoSample() {
    // 采样 + 刷新任务栏窗口
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

void App::HandleCommand(UINT cmd) {
    switch (cmd) {
        case IDM_RUN_STARTUP:  ToggleStartup(); break;
        case IDM_TOGGLE_TRAY:  ToggleTrayIcon(); break;
        case IDM_OPEN_TASKMGR: OpenTaskManager(); break;
        case IDM_ABOUT:        ShowAbout(); break;
        case IDM_EXIT:         Exit(); break;
    }
}

void App::ShowContextMenu(POINT pt) {
    UINT cmd = tray_.PopupMenu(hidden_hwnd_, pt, cfg_.run_on_startup,
                               cfg_.show_tray_icon);
    if (cmd != 0) HandleCommand(cmd);
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
        << L"↑ " << FormatSpeed(m.net_up_bps, true)
        << L"  ↓ " << FormatSpeed(m.net_down_bps, true)
        << L"\nCPU " << m.cpu_usage << L"%"
        << L"  内存 " << m.memory_usage << L"%";
    tray_.SetTip(oss.str().c_str());
}

} // namespace mm
