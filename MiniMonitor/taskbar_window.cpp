// taskbar_window.cpp — 任务栏窗口实现
#include "taskbar_window.h"
#include <algorithm>
#include <windowsx.h>

namespace mm {

namespace {

// Explorer 的 TrayNotifyWnd 是当前任务栏实际使用的托盘/时钟容器。
// 显示层的右边缘必须停在它左侧，不能再用一个固定像素值猜测。
constexpr int kTrayGap = 4;
constexpr int kRowSpace = 6;

HWND FindTrayNotifyWindow(HWND taskbar) {
    if (!taskbar) return nullptr;
    return FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
}

} // namespace

// Win11 主题注册表路径（用户级，无需管理员）
static const wchar_t* kPersonalizeKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

TaskbarWindow::~TaskbarWindow() {
    Destroy();
}

bool TaskbarWindow::Create(HINSTANCE hinst, const Config& cfg) {
    inst_ = hinst;
    cfg_  = cfg;

    // 注册窗口类。CS_HREDRAW|CS_VREDRAW：尺寸变就重画。
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &TaskbarWindow::WndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;                  // 自己画背景（双缓冲）
    wc.lpszClassName = ClassName();
    // 用类小图标，避免任务栏按钮（我们用了 WS_EX_TOOLWINDOW，本就不会有）
    RegisterClassExW(&wc);

    // 关键扩展样式（Win11 视觉嵌入的核心）：
    //   WS_EX_NOACTIVATE   点击不激活本窗口；跨进程点击穿透仍由 WM_NCHITTEST 尝试处理
    //   WS_EX_TOOLWINDOW   不出现在 Alt+Tab / 任务栏按钮
    //   WS_EX_TOPMOST      始终在最前（贴在任务栏上方层级）
    //   WS_EX_LAYERED      分层窗口，可整体透明、防闪烁
    //   WS_EX_TRANSPARENT  主要影响绘制顺序，不等同于可靠的跨进程点击穿透
    DWORD ex_style = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST
                   | WS_EX_LAYERED;
    DWORD style = WS_POPUP;

    hwnd_ = CreateWindowExW(
        ex_style, ClassName(), L"MiniMonitor",
        style,
        0, 0, 200, 32,         // 初始位置大小，Reposition 会重算
        nullptr, nullptr, hinst, this);

    if (!hwnd_) return false;

    // —— 分层窗口透明方案 ——
    // transparent=true 时用颜色键挖掉背景，只留下文字；否则用完全不透明
    // 的分层窗口，保证 Config.transparent 这个开关与实际行为一致。
    SetLayeredWindowAttributes(hwnd_, cfg_.back_color, 255,
                               cfg_.transparent ? LWA_COLORKEY : LWA_ALPHA);

    // 先建字体再定位，使测量结果与实际绘制字体一致；即使字体创建失败，
    // Reposition/DrawContent 也会回退到 DEFAULT_GUI_FONT。
    RecreateFont();
    Reposition();
    // 监控文字始终显示；设置统一通过显示区域右键菜单完成。
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    return true;
}

void TaskbarWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) { DeleteObject(font_); font_ = nullptr; }
}

void TaskbarWindow::RecreateFont() {
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    // 字号：pt → 像素。负数表示用磅为单位（Windows 约定）。
    HDC screen = GetDC(nullptr);
    const int dpi_y = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    int height = -MulDiv(cfg_.font_size, dpi_y > 0 ? dpi_y : 96, 72);
    DWORD weight = cfg_.font_bold ? FW_BOLD : FW_NORMAL;
    font_ = CreateFontW(
        height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        cfg_.font_name.c_str());
}

bool TaskbarWindow::IsDarkTheme() {
    // 读 AppsUseLightTheme：0=深色，1=浅色。任务栏通常跟随系统主题。
    HKEY hkey = nullptr;
    DWORD value = 1;       // 默认假设浅色（保守）
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0, KEY_READ, &hkey)
        == ERROR_SUCCESS) {
        DWORD data = 0, size = sizeof(data);
        if (RegQueryValueExW(hkey, L"SystemUsesLightTheme", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&data), &size) == ERROR_SUCCESS) {
            // 任务栏属于系统 UI，应优先跟随 SystemUsesLightTheme；
            // AppsUseLightTheme 只表示应用主题，可能与任务栏相反。
            value = data;
        } else {
            size = sizeof(data);
            if (RegQueryValueExW(hkey, L"AppsUseLightTheme", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(&data), &size) == ERROR_SUCCESS) {
                value = data;
            }
        }
        RegCloseKey(hkey);
    }
    return value == 0;   // light=1 → 非深色；light=0 → 深色
}

RECT TaskbarWindow::CalcTaskbarRect() {
    // 找到 Shell_TrayWnd（任务栏窗口）。无论 Win10/Win11 都是这个类名。
    RECT rc{ 0, 0, 0, 0 };
    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTaskbar || !GetWindowRect(hTaskbar, &rc)) {
        // 找不到就用主显示器底部 48px 兜底
        RECT desk{ 0, 0, GetSystemMetrics(SM_CXSCREEN),
                   GetSystemMetrics(SM_CYSCREEN) };
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &desk, 0);
        rc.left   = desk.left;
        rc.right  = desk.right;
        rc.bottom = desk.bottom;
        rc.top    = desk.bottom - 48;
    }
    return rc;
}

RECT TaskbarWindow::CalcDesiredRect(int content_w, int content_h) {
    RECT taskbar = CalcTaskbarRect();
    int taskbar_h = taskbar.bottom - taskbar.top;

    // 垂直居中于任务栏（Win11 任务栏高约 48px，内容 ~16px，需居中）
    int y = taskbar.top + (taskbar_h - content_h) / 2 + cfg_.window_offset_y;

    int x;
    if (cfg_.show_on_left) {
        // 左侧：贴开始按钮右边（Win11 开始按钮偏右一些，留 ~48px）
        x = taskbar.left + 48 + cfg_.window_offset_x;
    } else {
        // 右侧：优先使用 Explorer 的实际托盘容器左边界。
        // taskbar_right_space 只作为额外的保守留白/找不到托盘窗口时的回退值。
        int right_edge = taskbar.right - cfg_.taskbar_right_space;
        HWND tray = FindTrayNotifyWindow(FindWindowW(L"Shell_TrayWnd", nullptr));
        RECT tray_rect{ 0, 0, 0, 0 };
        if (tray && GetWindowRect(tray, &tray_rect)) {
            right_edge = (std::min)(right_edge,
                                    static_cast<int>(tray_rect.left) - kTrayGap);
        }

        x = right_edge - content_w + cfg_.window_offset_x;
        // 正向微调不能把窗口重新推回托盘区；负向微调仍然允许用户留出更多空间。
        if (x + content_w > right_edge) x = right_edge - content_w;
    }
    return RECT{ x, y, x + content_w, y + content_h };
}

void TaskbarWindow::Reposition() {
    if (!hwnd_) return;
    // 先用临时 DC 测量内容尺寸
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    SIZE sz{ 1, 1 };
    if (dc) {
        HGDIOBJ draw_font = font_ ? static_cast<HGDIOBJ>(font_)
                                  : GetStockObject(DEFAULT_GUI_FONT);
        HGDIOBJ old_font = SelectObject(dc, draw_font);
        // 用零值测一次宽高即可
        Metrics m = has_metrics_ ? last_metrics_ : Metrics{};
        sz = DrawContent(dc, m);   // 注意：这里会画到 dc，但 dc 是临时的，无所谓
        if (old_font) SelectObject(dc, old_font);
        DeleteDC(dc);
    }
    if (screen) ReleaseDC(nullptr, screen);

    if (sz.cx <= 0) sz.cx = 1;
    if (sz.cy <= 0) sz.cy = 1;

    RECT rc = CalcDesiredRect(sz.cx, sz.cy);
    // 用 SWP_NOZORDER 保持 TOPMOST，SWP_NOACTIVATE 不抢焦点。
    // 不要因为显示器变化而把用户手动隐藏的窗口重新显示出来。
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (IsWindowVisible(hwnd_)) flags |= SWP_SHOWWINDOW;
    SetWindowPos(hwnd_, nullptr, rc.left, rc.top,
                 rc.right - rc.left, rc.bottom - rc.top, flags);
}

void TaskbarWindow::Refresh(Monitor& monitor) {
    if (!hwnd_) return;
    monitor_ = &monitor;
    last_metrics_ = monitor.Update();
    has_metrics_ = true;
    // 速率单位可能从 B/s 变成 KB/MB/GB/s，文本宽度随指标变化；
    // 重新测量，避免新增字符把 CPU/内存段裁出窗口客户区。
    Reposition();
    InvalidateRect(hwnd_, nullptr, FALSE);   // 触发 WM_PAINT（不擦背景，双缓冲自己处理）
}

void TaskbarWindow::ApplyConfig(const Config& cfg) {
    cfg_ = cfg;
    RecreateFont();
    Reposition();
    // 颜色键可能变了（back_color 改动），重设分层属性
    if (hwnd_) {
        SetLayeredWindowAttributes(hwnd_, cfg_.back_color, 255,
                                   cfg_.transparent ? LWA_COLORKEY : LWA_ALPHA);
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// —— 绘制：把两列两行内容画到给定内存 DC，返回总宽高 ——
// 布局：左列为 ↑/↓，右列为 CPU/内存。
// 每个指标 = 标签(固定颜色) + 数值(可选不同颜色，这里统一)。
SIZE TaskbarWindow::DrawContent(HDC dc, const Metrics& m) {
    // 决定实际文字颜色（自动主题）
    COLORREF text = cfg_.text_color;
    if (cfg_.auto_theme) {
        bool dark = IsDarkTheme();
        // 深色任务栏 → 浅色字；浅色任务栏 → 深色字
        text = dark ? RGB(255, 255, 255) : RGB(32, 32, 32);
    }

    // 准备四段文本
    std::wstring up_text   = cfg_.up_str   + FormatSpeed(m.net_up_bps,
                                                         cfg_.short_speed_unit,
                                                         cfg_.separate_unit_space);
    std::wstring down_text = cfg_.down_str + FormatSpeed(m.net_down_bps,
                                                         cfg_.short_speed_unit,
                                                         cfg_.separate_unit_space);
    std::wstring cpu_text  = cfg_.cpu_str  + FormatPercent(m.cpu_usage,    cfg_.hide_percent);
    std::wstring mem_text  = cfg_.mem_str  + FormatPercent(m.memory_usage, cfg_.hide_percent);

    // 两列两行：左列为上行/下行，右列为 CPU/内存。
    const std::wstring* parts[2][2] = {
        { &up_text,   &cpu_text },
        { &down_text, &mem_text }
    };

    int col_widths[2] = { 0, 0 };
    int row_heights[2] = { 0, 0 };
    HGDIOBJ draw_font = font_ ? static_cast<HGDIOBJ>(font_)
                              : GetStockObject(DEFAULT_GUI_FONT);
    SelectObject(dc, draw_font);
    SetMapMode(dc, MM_TEXT);
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            const std::wstring& part = *parts[row][col];
            SIZE sz{ 0, 0 };
            GetTextExtentPoint32W(dc, part.c_str(),
                static_cast<int>(part.size()), &sz);
            col_widths[col] = (std::max)(col_widths[col], static_cast<int>(sz.cx));
            row_heights[row] = (std::max)(row_heights[row], static_cast<int>(sz.cy));
        }
    }
    // 最小列间距设为 20px，避免旧 INI 中的 item_space=8 让两列再次贴在一起；
    // 用户仍可在 INI 中设置更大的值。
    const int column_space = (std::max)(20, cfg_.item_space);
    const int row_space = cfg_.item_space > 0 ? kRowSpace : 0;
    const int total_w = col_widths[0] + column_space + col_widths[1];
    const int total_h = row_heights[0] + row_space + row_heights[1];

    // OnPaint 会先填充 back_color；transparent=true 时该颜色由颜色键挖空，
    // transparent=false 时由 LWA_ALPHA(255) 保留为实色背景。

    // 逐段绘制。窗口已经按测量结果调整尺寸，因此不启用裁剪选项，
    // 避免传入空裁剪矩形造成不同 GDI 实现的行为差异。
    SetBkMode(dc, TRANSPARENT);
    int y = 0;
    for (int row = 0; row < 2; ++row) {
        int x = 0;
        for (int col = 0; col < 2; ++col) {
            const std::wstring& part = *parts[row][col];
            // 上下用不同颜色让一眼可辨（上传偏黄、下载偏绿），仅在自动主题下做点缀
            COLORREF seg_color = text;
            if (cfg_.auto_theme && col == 0) {
                if (row == 0) seg_color = RGB(255, 200, 80);    // 上传 黄
                if (row == 1) seg_color = RGB(120, 220, 140);   // 下载 绿
            }
            SetTextColor(dc, seg_color);
            ExtTextOutW(dc, x, y, 0, nullptr, part.c_str(),
                        static_cast<UINT>(part.size()), nullptr);
            x += col_widths[col] + (col == 0 ? column_space : 0);
        }
        y += row_heights[row] + row_space;
    }

    return SIZE{ total_w, total_h };
}

void TaskbarWindow::OnPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);

    // —— 双缓冲：画到内存位图，再 BitBlt 到屏幕，防闪烁 ——
    RECT rc_client;
    GetClientRect(hwnd_, &rc_client);
    int w = rc_client.right - rc_client.left;
    int h = rc_client.bottom - rc_client.top;
    if (w <= 0 || h <= 0) { EndPaint(hwnd_, &ps); return; }

    HDC mem_dc  = CreateCompatibleDC(dc);
    HBITMAP mem_bmp = CreateCompatibleBitmap(dc, w, h);
    if (!mem_dc || !mem_bmp) {
        if (mem_bmp) DeleteObject(mem_bmp);
        if (mem_dc) DeleteDC(mem_dc);
        EndPaint(hwnd_, &ps);
        return;
    }
    HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);
    HGDIOBJ draw_font = font_ ? static_cast<HGDIOBJ>(font_)
                              : GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ old_font = SelectObject(mem_dc, draw_font);

    // 背景
    HBRUSH bg = CreateSolidBrush(cfg_.back_color);
    FillRect(mem_dc, &rc_client, bg);
    DeleteObject(bg);

    // 内容（用缓存的最近一次指标，避免每次重绘都采样）
    Metrics m = has_metrics_ ? last_metrics_ : Metrics{};
    DrawContent(mem_dc, m);

    // 拷到屏幕
    BitBlt(dc, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);

    // 清理 GDI 对象（避免泄漏，长跑必须）
    SelectObject(mem_dc, old_font);
    SelectObject(mem_dc, old_bmp);
    DeleteObject(mem_bmp);
    DeleteDC(mem_dc);

    EndPaint(hwnd_, &ps);
}

void TaskbarWindow::OnTimer() {
    // 实际采样在 App 层做（App 持有 Monitor）。
    // 这里只触发重绘；若 App 没调用 Refresh，则用旧指标重画。
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TaskbarWindow::OnDestroy() {
    hwnd_ = nullptr;
}

LRESULT TaskbarWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TaskbarWindow* self = nullptr;
    // 首次收到 WM_NCCREATE 时，把 this 指针存入窗口用户数据，后续取出。
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lp);
        self = reinterpret_cast<TaskbarWindow*>(cs->lpCreateParams);
        if (self) {
            // WM_NCCREATE 期间成员 hwnd_ 仍为空；先写入真实句柄，避免
            // HandleMessage 的默认分支把 nullptr 传给 DefWindowProcW。
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    } else {
        self = reinterpret_cast<TaskbarWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TaskbarWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:      OnPaint(); return 0;
        case WM_TIMER:      OnTimer(); return 0;
        case WM_ERASEBKGND: return 1;   // 告诉系统"我已擦背景"，避免闪烁
        case WM_NCHITTEST:
            // 监控窗口用颜色键透明：文字像素可命中，背景（颜色键）像素本就穿透。
            // 这里统一返回 HTTRANSPARENT，让普通鼠标点击都穿到任务栏；右键由
            // App 的轻量鼠标钩子单独拦截并弹出设置菜单。这样左键不挡任务栏操作，
            // 右键又能稳定打开设置。
            return HTTRANSPARENT;
        case WM_RBUTTONUP: {
            POINT pt{};
            if (context_menu_target_ && GetCursorPos(&pt)) {
                PostMessageW(context_menu_target_, WM_MINIMONITOR_CONTEXT_MENU,
                             0, MAKELPARAM(pt.x, pt.y));
            }
            return 0;
        }
        case WM_DESTROY:    OnDestroy(); return 0;
        // 分辨率/多显示器变化：重新定位
        case WM_DISPLAYCHANGE:
            Reposition();
            return 0;
        // DPI 变化（用户在显示器间拖动）：重建字体+重定位
        case WM_DPICHANGED: {
            RecreateFont();
            // 不直接采用系统建议矩形；重新读取托盘边界，避免 DPI 切换后覆盖图标。
            Reposition();
            return 0;
        }
        // 任务栏显示层只处理右键菜单，其余交互保持默认行为。
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace mm
