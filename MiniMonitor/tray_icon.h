// tray_icon.h — 系统托盘图标 + 右键菜单
#pragma once
#include <windows.h>
#include <shellapi.h>

namespace mm {

// 菜单项 ID（必须 < 0x8000 以避免和系统冲突；此处用小整数）
enum : UINT {
    IDM_RUN_STARTUP = 2002,
    IDM_OPEN_TASKMGR= 2003,
    IDM_ABOUT       = 2004,
    IDM_EXIT        = 2005,
    IDM_TOGGLE_TRAY = 2006,
};

// 自定义消息：托盘图标回调
inline constexpr UINT WM_TRAYICON = WM_USER + 1;

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();

    // 添加托盘图标。callback_hwnd 是接收 WM_TRAYICON 的窗口。
    bool Add(HINSTANCE hinst, HWND callback_hwnd, UINT callback_msg,
             HICON icon, const wchar_t* tip);

    // 更新工具提示
    void SetTip(const wchar_t* tip);

    // 在指定屏幕坐标弹出设置菜单，返回被点选的命令 ID（0 表示没选）。
    UINT PopupMenu(HWND hwnd, POINT pt, bool run_on_startup,
                   bool show_tray_icon);

    // 菜单打开期间由 Raw Input 观察器调用；只结束菜单，不吞掉外部鼠标消息。
    void CancelMenu();
    bool IsMenuOpen() const { return menu_open_; }
    bool IsPointInMenu(POINT pt) const;

    void Remove();

    bool IsAdded() const { return added_; }

private:
    NOTIFYICONDATAW nid_{};
    bool added_ = false;
    bool menu_open_ = false;
};

} // namespace mm
