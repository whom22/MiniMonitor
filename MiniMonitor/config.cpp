// config.cpp — INI 配置读写实现
#include "config.h"
#include <climits>
#include <cwchar>
#include <string>

namespace mm {

// 把 COLORREF 拆成 "R,G,B" 字符串（INI 里可读，避免 BGR/RGB 混淆）。
static std::wstring ColorToString(COLORREF c) {
    return std::to_wstring(GetRValue(c)) + L',' +
           std::to_wstring(GetGValue(c)) + L',' +
           std::to_wstring(GetBValue(c));
}
// 解析 "R,G,B"。失败返回默认色。
static COLORREF StringToColor(const std::wstring& s, COLORREF fallback) {
    int r = -1, g = -1, b = -1;
    // 用 swscanf 容错解析
    if (swscanf_s(s.c_str(), L"%d,%d,%d", &r, &g, &b) == 3 &&
        r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
        return RGB(r, g, b);
    }
    return fallback;
}

void ConfigManager::Init() {
    // 取 exe 自身路径，去掉文件名，拼上 MiniMonitor.ini。
    // 这样程序放哪、配置就在哪，绿色便携。
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos + 1);
    else dir = L".\\";
    path_ = dir + L"MiniMonitor.ini";
}

// —— 读取的小封装 ——
// 注意：GetPrivateProfileInt 不支持负数，而 window_offset_x/y 可能为负，
// 所以整数统一走字符串读 + wcstol，可正确处理 "-5" 这样的值并拒绝非法文本。
static int GetInt(LPCWSTR app, LPCWSTR key, int def, const std::wstring& file) {
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(app, key, nullptr, buf, 32, file.c_str());
    if (buf[0] == 0) return def;          // 键不存在 → 默认值
    wchar_t* end = nullptr;
    const long value = wcstol(buf, &end, 10);
    if (end == buf || (*end != L' ' && *end != L'\t' && *end != L'\0') ||
        value < INT_MIN || value > INT_MAX) {
        return def;
    }
    return static_cast<int>(value);
}
static bool GetBool(LPCWSTR app, LPCWSTR key, bool def, const std::wstring& file) {
    return GetInt(app, key, def ? 1 : 0, file) != 0;
}
static std::wstring GetStr(LPCWSTR app, LPCWSTR key, const std::wstring& def,
                           const std::wstring& file) {
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(app, key, def.c_str(), buf, 512, file.c_str());
    return buf;
}
static COLORREF GetColor(LPCWSTR app, LPCWSTR key, COLORREF def,
                         const std::wstring& file) {
    std::wstring def_s = ColorToString(def);
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(app, key, def_s.c_str(), buf, 64, file.c_str());
    return StringToColor(buf, def);
}

Config ConfigManager::Load() {
    Config c;   // 默认值兜底
    if (path_.empty()) Init();
    const std::wstring& f = path_;

    c.sample_interval_ms = GetInt (L"General", L"sample_interval_ms", c.sample_interval_ms, f);

    c.font_name = GetStr(L"Display", L"font_name", c.font_name, f);
    c.font_size = GetInt(L"Display", L"font_size", c.font_size, f);
    c.font_bold = GetBool(L"Display", L"font_bold", c.font_bold, f);
    c.text_color  = GetColor(L"Display", L"text_color",  c.text_color,  f);
    c.back_color  = GetColor(L"Display", L"back_color",  c.back_color,  f);
    c.transparent = GetBool(L"Display", L"transparent",  c.transparent, f);
    c.auto_theme  = GetBool(L"Display", L"auto_theme",   c.auto_theme,  f);

    c.up_str   = GetStr(L"Display", L"up_str",   c.up_str,   f);
    c.down_str = GetStr(L"Display", L"down_str", c.down_str, f);
    c.cpu_str  = GetStr(L"Display", L"cpu_str",  c.cpu_str,  f);
    c.mem_str  = GetStr(L"Display", L"mem_str",  c.mem_str,  f);

    c.short_speed_unit    = GetBool(L"Display", L"short_speed_unit",    c.short_speed_unit,    f);
    c.hide_percent        = GetBool(L"Display", L"hide_percent",        c.hide_percent,        f);
    c.separate_unit_space = GetBool(L"Display", L"separate_unit_space", c.separate_unit_space, f);
    c.item_space          = GetInt (L"Display", L"item_space",          c.item_space,          f);

    c.taskbar_right_space = GetInt(L"Taskbar", L"taskbar_right_space", c.taskbar_right_space, f);
    c.window_offset_x     = GetInt(L"Taskbar", L"window_offset_x",     c.window_offset_x,     f);
    c.window_offset_y     = GetInt(L"Taskbar", L"window_offset_y",     c.window_offset_y,     f);
    c.show_on_left        = GetBool(L"Taskbar", L"show_on_left",       c.show_on_left,        f);

    c.run_on_startup = GetBool(L"Behavior", L"run_on_startup", c.run_on_startup, f);
    c.show_tray_icon = GetBool(L"Behavior", L"show_tray_icon", c.show_tray_icon, f);

    // 防止手工编辑 INI 时把刷新周期/字号/间距设成异常值，造成高频
    // 采样、窗口尺寸异常或负数布局。默认值仍保持 1 秒，满足低占用目标。
    if (c.sample_interval_ms < 100) c.sample_interval_ms = 100;
    if (c.sample_interval_ms > 60000) c.sample_interval_ms = 60000;
    if (c.font_size < 6) c.font_size = 6;
    if (c.font_size > 48) c.font_size = 48;
    // 当前布局需要足够的列间距；兼容旧 INI 时也不允许恢复成过窄的 8/20px。
    if (c.item_space < 28) c.item_space = 28;
    if (c.item_space > 64) c.item_space = 64;
    if (c.taskbar_right_space < 0) c.taskbar_right_space = 0;
    if (c.taskbar_right_space > 2000) c.taskbar_right_space = 2000;

    return c;
}

// —— 写入的小封装 ——
static void PutInt (LPCWSTR app, LPCWSTR key, int v,           const std::wstring& f) {
    WritePrivateProfileStringW(app, key, std::to_wstring(v).c_str(), f.c_str());
}
static void PutBool(LPCWSTR app, LPCWSTR key, bool v,          const std::wstring& f) {
    WritePrivateProfileStringW(app, key, v ? L"1" : L"0", f.c_str());
}
static void PutStr (LPCWSTR app, LPCWSTR key, const std::wstring& v, const std::wstring& f) {
    WritePrivateProfileStringW(app, key, v.c_str(), f.c_str());
}

void ConfigManager::Save(const Config& c) {
    if (path_.empty()) Init();
    const std::wstring& f = path_;

    PutInt (L"General", L"sample_interval_ms", c.sample_interval_ms, f);

    PutStr (L"Display", L"font_name", c.font_name, f);
    PutInt (L"Display", L"font_size", c.font_size, f);
    PutBool(L"Display", L"font_bold", c.font_bold, f);
    PutStr (L"Display", L"text_color",  ColorToString(c.text_color), f);
    PutStr (L"Display", L"back_color",  ColorToString(c.back_color), f);
    PutBool(L"Display", L"transparent", c.transparent, f);
    PutBool(L"Display", L"auto_theme",  c.auto_theme,  f);

    PutStr (L"Display", L"up_str",   c.up_str,   f);
    PutStr (L"Display", L"down_str", c.down_str, f);
    PutStr (L"Display", L"cpu_str",  c.cpu_str,  f);
    PutStr (L"Display", L"mem_str",  c.mem_str,  f);

    PutBool(L"Display", L"short_speed_unit",    c.short_speed_unit,    f);
    PutBool(L"Display", L"hide_percent",        c.hide_percent,        f);
    PutBool(L"Display", L"separate_unit_space", c.separate_unit_space, f);
    PutInt (L"Display", L"item_space",          c.item_space,          f);

    PutInt (L"Taskbar", L"taskbar_right_space", c.taskbar_right_space, f);
    PutInt (L"Taskbar", L"window_offset_x",     c.window_offset_x,     f);
    PutInt (L"Taskbar", L"window_offset_y",     c.window_offset_y,     f);
    PutBool(L"Taskbar", L"show_on_left",        c.show_on_left,        f);

    PutBool(L"Behavior", L"run_on_startup", c.run_on_startup, f);
    PutBool(L"Behavior", L"show_tray_icon", c.show_tray_icon, f);
    // 清理旧版本的隐藏窗口配置，避免用户继续误以为该功能仍有效。
    WritePrivateProfileStringW(L"Behavior", L"hide_window", nullptr, f.c_str());
}

} // namespace mm
