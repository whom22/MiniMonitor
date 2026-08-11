// monitor.h — 数据采集：网络流量 / CPU / 内存
// 纯 Win32 API，零第三方依赖。
#pragma once
#include <windows.h>
#include <string>

namespace mm {

// 一次采样得到的原始指标快照。
struct Metrics {
    // 网络速率（字节/秒），由两次采样差值计算
    ULONGLONG net_up_bytes_per_sec = 0;     // 上行，字节/秒
    ULONGLONG net_down_bytes_per_sec = 0;   // 下行，字节/秒
    // CPU 占用百分比（0~100），整数
    int cpu_usage = 0;
    // 内存占用百分比（0~100）
    int memory_usage = 0;
    // 累计流量（字节），用于显示"今日总量"等扩展
    ULONGLONG total_up = 0;
    ULONGLONG total_down = 0;
    // 系统总物理内存（MB），用于扩展显示
    ULONGLONG total_memory_mb = 0;
    ULONGLONG avail_memory_mb = 0;
};

// 采集器。每个 App 持有一个实例。
// 设计：Update() 由主窗口定时器每秒调用一次；
// 内部维护上一次的系统计数，差分出速率。
class Monitor {
public:
    Monitor() = default;
    ~Monitor() = default;

    // 首次调用：仅记录基线，返回的速率会是 0。
    // 之后每次调用：差分出 1 秒级速率。
    // 采样本身耗时 <0.1ms，可在 UI 线程直接调用。
    Metrics Update();

private:
    // —— 网络采样 ——
    // 累加所有活动物理网卡（排除 loopback/tunnel/虚拟过滤接口）的
    // InOctets/OutOctets。用 GetIfTable2 + FreeMibTable（含 64 位计数）。
    bool SampleNetwork(ULONGLONG& out_total_in, ULONGLONG& out_total_out);

    // —— CPU 采样 ——
    // 用 GetSystemTimes 拿 idle/kernel/user 三个 FILETIME，
    // 差分得到占用率。这是 TrafficMonitor 同款做法。
    bool SampleCpu(ULONGLONG& out_idle, ULONGLONG& out_kernel, ULONGLONG& out_user);

    // —— 内存采样 ——
    // GlobalMemoryStatusEx 的 dwMemoryLoad 直接给占用百分比。
    void SampleMemory(int& out_usage, ULONGLONG& out_total_mb, ULONGLONG& out_avail_mb);

    // 网络/CPU 分别维护有效基线。某次系统 API 失败时，不让无效的 0
    // 计数参与下一次差分，避免出现一次异常的超大网速或 CPU=100%。
    bool     has_network_baseline_ = false;
    bool     has_cpu_baseline_ = false;

    // 上一次采样的单调时钟，用于把任意采样间隔换算为真正的每秒速率。
    ULONGLONG last_sample_tick_ = 0;

    // 网络基线
    ULONGLONG prev_in_ = 0;
    ULONGLONG prev_out_ = 0;
    // CPU 基线
    ULONGLONG prev_idle_ = 0;
    ULONGLONG prev_kernel_ = 0;
    ULONGLONG prev_user_ = 0;
    // 累计流量（自程序启动起）
    ULONGLONG acc_up_ = 0;
    ULONGLONG acc_down_ = 0;
};

// 把字节/秒格式化成 "1.23 MB/s" 这类带单位的可读字符串。
// short_mode=false 时显示单位，true 时只显示数字（更省宽度）。
std::wstring FormatSpeed(ULONGLONG bytes_per_sec, bool short_mode = false,
                         bool separate_unit_space = true);

// 把占用百分比格式化成 "12%" 或 "12"（无符号）。
std::wstring FormatPercent(int percent, bool hide_percent = false);

} // namespace mm
