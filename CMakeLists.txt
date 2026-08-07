# CMakeLists.txt — MiniMonitor 备选构建
#
# 用法：
#   cmake -B build -S . -A x64
#   cmake --build build --config Release
#   生成的 exe: build/Release/MiniMonitor.exe
#
# 要求：CMake 3.20+，Visual Studio 2022（或 Build Tools）含 C++ 桌面开发。
cmake_minimum_required(VERSION 3.20)
project(MiniMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# —— 源文件 ——
set(SOURCES
    MiniMonitor/main.cpp
    MiniMonitor/app.cpp
    MiniMonitor/monitor.cpp
    MiniMonitor/taskbar_window.cpp
    MiniMonitor/tray_icon.cpp
    MiniMonitor/config.cpp
)
set(HEADERS
    MiniMonitor/app.h
    MiniMonitor/monitor.h
    MiniMonitor/taskbar_window.h
    MiniMonitor/tray_icon.h
    MiniMonitor/config.h
    MiniMonitor/resource.h
)

add_executable(MiniMonitor WIN32 ${SOURCES} ${HEADERS}
    MiniMonitor/app.rc)

target_include_directories(MiniMonitor PRIVATE MiniMonitor)

target_compile_definitions(MiniMonitor PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000)

# —— 链接库 ——
# Win32 API libraries: 网络流量、注册表、Shell/托盘、窗口与 GDI
target_link_libraries(MiniMonitor PRIVATE
    iphlpapi advapi32 shell32 user32 gdi32)

# —— 编译选项 ——
if(MSVC)
    target_compile_definitions(MiniMonitor PRIVATE _CRT_SECURE_NO_WARNINGS)
    # 静态链接 CRT（MSVC_RUNTIME_LIBRARY 需 CMake 3.15+）
    set_property(TARGET MiniMonitor PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Release 优化体积
    target_compile_options(MiniMonitor PRIVATE /W3 /permissive-)
    target_link_options(MiniMonitor PRIVATE
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>)
elseif(MINGW)
    # main.cpp 使用 wWinMain；MinGW 需要显式选择 Unicode GUI 启动代码。
    # 把 MinGW 的 libgcc/libstdc++/winpthread 静态打进 exe，避免发布目录
    # 还必须携带 libwinpthread-1.dll；Windows 系统 DLL 仍按系统提供。
    target_link_options(MiniMonitor PRIVATE
        -municode
        -static
        -static-libgcc
        -static-libstdc++)
    target_compile_definitions(MiniMonitor PRIVATE MINIMONITOR_MINGW)
endif()

# 输出到 build/bin
set_target_properties(MiniMonitor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
