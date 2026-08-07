// main.cpp — 程序入口
//
// 设计要点（追求"占用最小、零打扰"）：
//   1. 单实例：已运行则直接退出，避免多开。
//   2. DPI 感知：让 Windows 不做位图缩放，文字清晰。
//   3. 静态链接 CRT（vcxproj 设 /MT）：单 exe，无 DLL 依赖。
//
#include <windows.h>
#include <shellapi.h>
#include <cstring>
#include "app.h"

// 入口。/SUBSYSTEM:WINDOWS → WinMain；不显示控制台。
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    PWSTR /*cmd_line*/, int cmd_show) {

    // —— 1. 单实例检测 ——
    // 用命名互斥量。已存在则把已运行实例的任务栏窗口提到前台（可选）后退出。
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\MiniMonitor_SingleInstance");
    if (!hMutex) {
        // 无法创建互斥量时继续运行会失去单实例保证，直接失败更安全。
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例在跑，直接退出，不打扰用户。
        CloseHandle(hMutex);
        return 0;
    }

    // —— 2. DPI 感知 ——
    // 优先用 SetProcessDpiAwarenessContext（Win10 1703+），回退到旧 API。
    // 这样在高 DPI 显示器上文字不会糊。
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        // SetProcessDpiAwarenessContext 函数指针动态获取，兼容旧系统
        typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
        FARPROC rawSetCtx = GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        PFN_SetCtx pSetCtx = nullptr;
        static_assert(sizeof(pSetCtx) == sizeof(rawSetCtx));
        std::memcpy(&pSetCtx, &rawSetCtx, sizeof(pSetCtx));
        if (pSetCtx) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((DPI_CONTEXT_HANDLE)-4)
            pSetCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
        } else {
            SetProcessDPIAware();
        }
    }

    // —— 3. 运行 ——
    mm::App app;
    int ret = app.Run(hInstance, cmd_show);

    if (hMutex) CloseHandle(hMutex);
    return ret;
}
