// tray_icon.cpp — 系统托盘图标 + 右键菜单实现
#include "tray_icon.h"

namespace mm {

TrayIcon::~TrayIcon() {
    Remove();
}

bool TrayIcon::Add(HINSTANCE hinst, HWND callback_hwnd, UINT callback_msg,
                   HICON icon, const wchar_t* tip) {
    UNREFERENCED_PARAMETER(hinst);
    // NOTIFYICONDATA0..3 各版本结构不同，这里用 V3（Vista+，Win11 完全支持）。
    nid_ = {};
    nid_.cbSize = sizeof(NOTIFYICONDATAW);
    nid_.hWnd   = callback_hwnd;
    nid_.uID    = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uCallbackMessage = callback_msg;
    nid_.hIcon  = icon ? icon : LoadIcon(nullptr, IDI_APPLICATION);
    if (tip) {
        wcsncpy_s(nid_.szTip, tip, _TRUNCATE);
    }
    added_ = Shell_NotifyIconW(NIM_ADD, &nid_) != 0;
    // 若 ADD 失败，某些情况下要先 DELETE 再 ADD（图标残留）
    if (!added_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        added_ = Shell_NotifyIconW(NIM_ADD, &nid_) != 0;
    }
    return added_;
}

void TrayIcon::SetTip(const wchar_t* tip) {
    if (!added_) return;
    nid_.uFlags = NIF_TIP;
    if (tip) wcsncpy_s(nid_.szTip, tip, _TRUNCATE);
    else nid_.szTip[0] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

UINT TrayIcon::PopupMenu(HWND hwnd, POINT pt, bool run_on_startup,
                         bool show_tray_icon) {
    // 创建一个临时弹出菜单。
    // 注意：托盘菜单不能用窗口主菜单方式，必须用 CreatePopupMenu + TrackPopupMenu。
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;
    AppendMenuW(menu, MF_STRING | (run_on_startup ? MF_CHECKED : MF_UNCHECKED),
                IDM_RUN_STARTUP, L"开机自启\t");
    AppendMenuW(menu, MF_STRING | (show_tray_icon ? MF_CHECKED : MF_UNCHECKED),
                IDM_TOGGLE_TRAY, L"显示托盘图标\t");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_TASKMGR, L"任务管理器...\t");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"关于 MiniMonitor\t");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出\t");

    // TrackPopupMenu 要求 owner 真正成为前台窗口，否则菜单可能一直捕获在
    // 当前应用上，点击其它窗口也不会消失。hidden_hwnd_ 平时不可见，因此在
    // 菜单期间临时显示为 1x1 的屏幕外工具窗口，让 SetForegroundWindow 生效。
    const bool owner_was_visible = IsWindowVisible(hwnd) != FALSE;
    if (!owner_was_visible) {
        SetWindowPos(hwnd, nullptr, -32000, -32000, 1, 1,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    if (GetForegroundWindow() != hwnd) {
        // Raw Input 是观察者路径，不一定会给本进程授予前台切换权。
        // 只在菜单即将打开的这一瞬间授予前台权限，再立即切换 owner；
        // 不改变其它窗口的输入传递。
        AllowSetForegroundWindow(ASFW_ANY);
        SetForegroundWindow(hwnd);
    }
    if (GetForegroundWindow() != hwnd) {
        // 同线程回退；正常情况下前面的显示动作已足够。
        SetActiveWindow(hwnd);
    }

    // TrackPopupMenu 返回的是被选项 ID，取消返回 0。
    // TPM_RETURNCMD 让它返回命令 ID 而不是发 WM_COMMAND。
    // TPM_NONOTIFY 不发 WM_MENUCOMMAND。
    // 右对齐：让菜单不超出屏幕右边（托盘在右下角）。
    menu_open_ = true;
    UINT cmd = TrackPopupMenuEx(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN,
        pt.x, pt.y, hwnd, nullptr);
    menu_open_ = false;

    // 配合 SetForegroundWindow 的已知 bug：发一个空消息唤醒
    PostMessageW(hwnd, WM_NULL, 0, 0);

    if (!owner_was_visible) {
        ShowWindow(hwnd, SW_HIDE);
    }

    DestroyMenu(menu);
    return cmd;
}

void TrayIcon::CancelMenu() {
    if (menu_open_) EndMenu();
}

bool TrayIcon::IsPointInMenu(POINT pt) const {
    if (!menu_open_) return false;

    // TrackPopupMenu 创建的标准菜单窗口类名固定为 #32768。
    // WindowFromPoint 能同时覆盖主菜单和打开后的子菜单，避免误关菜单项点击。
    HWND hit = WindowFromPoint(pt);
    if (!hit) return false;

    wchar_t class_name[32] = {};
    return GetClassNameW(hit, class_name,
                         static_cast<int>(sizeof(class_name) /
                                          sizeof(class_name[0]))) > 0
        && wcscmp(class_name, L"#32768") == 0;
}

void TrayIcon::Remove() {
    if (added_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        added_ = false;
    }
}

} // namespace mm
