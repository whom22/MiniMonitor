// monitor.cpp — 数据采集实现
#include "monitor.h"
// —— IP Helper / 网络统计相关头文件 ——
// 注意包含顺序：netioapi.h 内部使用 NTSTATUS 类型，但 MSVC 的 <windows.h>
// 默认不引入 NTSTATUS（MinGW 头文件会顺带定义，所以此前未暴露）。
// <winternl.h> 是引入 NTSTATUS 的正规用户态头文件，且不会与 <windows.h>
// 产生类型重定义冲突（<ntdef.h> 会，故不用）。
#include <winsock2.h>
#include <ws2ipdef.h>
#include <winternl.h>
#include <netioapi.h>
#include <iphlpapi.h>
#include <sstream>
#include <iomanip>
#include <limits>

#if defined(_MSC_VER)
#pragma comment(lib, "iphlpapi.lib")
#endif

namespace mm {

// MIB_IF_ROW2::InOctets/OutOctets 的单位是 octet，即 byte，不是 bit。
// 这里单独封装差分换算，避免后续改动把网络链路速率（bit/s）混入流量值。
static ULONGLONG BytesPerSecond(ULONGLONG delta_bytes,
                                ULONGLONG elapsed_ms) {
    if (elapsed_ms == 0) return 0;
    const ULONGLONG max_value = (std::numeric_limits<ULONGLONG>::max)();
    const ULONGLONG whole = delta_bytes / elapsed_ms;
    const ULONGLONG remainder = delta_bytes % elapsed_ms;
    if (whole > max_value / 1000) return max_value;
    const ULONGLONG scaled_remainder =
        (remainder * 1000) / elapsed_ms;
    return whole * 1000 > max_value - scaled_remainder
        ? max_value
        : whole * 1000 + scaled_remainder;
}

// 将 FILETIME（两段 32 位）转换为单个 64 位 tick 数。
// GetSystemTimes 返回的是 100ns 间隔的计数，但我们只用差分比值，
// 不需要换算成秒，直接当整数处理即可。
static inline ULONGLONG FileTimeToU64(const FILETIME& ft) {
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool Monitor::SampleNetwork(ULONGLONG& out_total_in, ULONGLONG& out_total_out) {
    out_total_in = 0;
    out_total_out = 0;

    PMIB_IF_TABLE2 table = nullptr;
    // GetIfTable2 分配内存（需用 FreeMibTable 释放），返回所有网卡。
    const auto status = GetIfTable2(&table);
    if (status != NO_ERROR || table == nullptr) {
        if (table) FreeMibTable(table);
        return false;
    }

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        // 跳过环回、断开、非物理网卡。IfType 取值见 ifmib.h：
        //   IF_TYPE_SOFTWARE_LOOPBACK = 24
        //   IF_TYPE_TUNNEL            = 131
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (row.Type == IF_TYPE_TUNNEL) continue;
        // OperStatus: 仅统计 Up 的网卡，避免把断开的虚拟网卡计入。
        if (row.OperStatus != IfOperStatusUp) continue;

        // GetIfTable2 还会返回 Hyper-V/VPN/过滤器等虚拟接口；同一份
        // 物理流量可能在这些接口上重复出现。只保留硬件接口，避免速率
        // 被重复累计（这也是显示值明显偏大的常见来源）。
        if (!row.InterfaceAndOperStatusFlags.HardwareInterface) continue;

        // InOctets = 下行（接收），OutOctets = 上行（发送）。
        // 64 位计数，不易溢出。
        out_total_in  += row.InOctets;
        out_total_out += row.OutOctets;
    }
    FreeMibTable(table);
    return true;
}

bool Monitor::SampleCpu(ULONGLONG& out_idle, ULONGLONG& out_kernel, ULONGLONG& out_user) {
    FILETIME ft_idle, ft_kernel, ft_user;
    // GetSystemTimes 是全局 CPU 时间（所有核合并），
    // 比 GetSystemTimeAsFileTime + 计算更轻、更准。
    if (GetSystemTimes(&ft_idle, &ft_kernel, &ft_user) == 0) {
        out_idle = out_kernel = out_user = 0;
        return false;
    }
    out_idle   = FileTimeToU64(ft_idle);
    out_kernel = FileTimeToU64(ft_kernel);   // kernel 含 idle，是"系统总"
    out_user   = FileTimeToU64(ft_user);
    return true;
}

void Monitor::SampleMemory(int& out_usage, ULONGLONG& out_total_mb, ULONGLONG& out_avail_mb) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        out_usage = 0;
        out_total_mb = out_avail_mb = 0;
        return;
    }
    out_usage     = static_cast<int>(ms.dwMemoryLoad);          // 0~100
    out_total_mb  = ms.ullTotalPhys / (1024 * 1024);
    out_avail_mb  = ms.ullAvailPhys / (1024 * 1024);
}

Metrics Monitor::Update() {
    Metrics m;

    const ULONGLONG now_tick = GetTickCount64();
    ULONGLONG elapsed_ms = 0;
    if (last_sample_tick_ != 0 && now_tick > last_sample_tick_)
        elapsed_ms = now_tick - last_sample_tick_;
    last_sample_tick_ = now_tick;

    // —— 网络 ——
    ULONGLONG cur_in = 0, cur_out = 0;
    ULONGLONG delta_in = 0, delta_out = 0;
    bool have_in_delta = false;
    bool have_out_delta = false;
    const bool network_ok = SampleNetwork(cur_in, cur_out);
    if (!network_ok) {
        has_network_baseline_ = false;
    } else {
        // 差分。注意计数器可能在系统睡眠/网卡重置后回绕或变小，做保护。
        if (has_network_baseline_ && cur_in >= prev_in_) {
            delta_in = cur_in - prev_in_;
            have_in_delta = true;
            m.net_down_bytes_per_sec = BytesPerSecond(delta_in, elapsed_ms);
        }
        if (has_network_baseline_ && cur_out >= prev_out_) {
            delta_out = cur_out - prev_out_;
            have_out_delta = true;
            m.net_up_bytes_per_sec = BytesPerSecond(delta_out, elapsed_ms);
        }
        prev_in_ = cur_in;
        prev_out_ = cur_out;
        has_network_baseline_ = true;
    }
    // 累计流量：把本秒增量加进来（首次也记）
    if (network_ok && has_network_baseline_ &&
        (have_in_delta || have_out_delta)) {
        const ULONGLONG max_value = (std::numeric_limits<ULONGLONG>::max)();
        if (have_in_delta) {
            acc_down_ = delta_in > max_value - acc_down_
                ? max_value : acc_down_ + delta_in;
        }
        if (have_out_delta) {
            acc_up_ = delta_out > max_value - acc_up_
                ? max_value : acc_up_ + delta_out;
        }
    }
    m.total_down = acc_down_;
    m.total_up   = acc_up_;

    // —— CPU ——
    ULONGLONG cur_idle = 0, cur_kernel = 0, cur_user = 0;
    const bool cpu_ok = SampleCpu(cur_idle, cur_kernel, cur_user);
    if (!cpu_ok) {
        has_cpu_baseline_ = false;
    } else if (has_cpu_baseline_ &&
               cur_idle >= prev_idle_ && cur_kernel >= prev_kernel_ && cur_user >= prev_user_) {
        ULONGLONG delta_sys    = (cur_kernel - prev_kernel_) + (cur_user - prev_user_);
        ULONGLONG delta_idle   = cur_idle - prev_idle_;
        // 注意 kernel 时间里已经包含了 idle，所以 "busy = (kernel-idle) + user"
        // 但 delta_kernel 已含 delta_idle，故分母用 (kernel+user) 的差分最稳。
        ULONGLONG denom = delta_sys;
        if (denom > 0) {
            if (delta_idle <= delta_sys) {
                ULONGLONG busy = delta_sys - delta_idle; // (Δkernel+Δuser) - Δidle
                // 限幅 0~100*100（用整数百分比，乘 100 避免浮点）
                if (busy > denom) busy = denom;
                m.cpu_usage = static_cast<int>(busy * 100 / denom);
                if (m.cpu_usage > 100) m.cpu_usage = 100;
            }
        }
    }
    if (cpu_ok) {
        prev_idle_   = cur_idle;
        prev_kernel_ = cur_kernel;
        prev_user_   = cur_user;
        has_cpu_baseline_ = true;
    }

    // —— 内存 ——
    SampleMemory(m.memory_usage, m.total_memory_mb, m.avail_memory_mb);

    return m;
}

// 单位换算：B/s → KB/s → MB/s → GB/s
// 阈值用 1024（二进制），符合 Windows 资源管理器的习惯。
std::wstring FormatSpeed(ULONGLONG bytes_per_sec, bool short_mode,
                         bool separate_unit_space) {
    const wchar_t* units[] = { L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s" };
    double v = static_cast<double>(bytes_per_sec);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }

    std::wostringstream oss;
    // 小于 10 保留两位，小于 100 保留一位，否则整数；保持列宽稳定
    if (u == 0) {
        oss << static_cast<ULONGLONG>(v);           // B/s 用整数
    } else if (v < 10.0) {
        oss << std::fixed << std::setprecision(2) << v;
    } else if (v < 100.0) {
        oss << std::fixed << std::setprecision(1) << v;
    } else {
        oss << static_cast<ULONGLONG>(v + 0.5);
    }
    if (!short_mode) {
        if (separate_unit_space) oss << L' ';
        oss << units[u];
    }
    return oss.str();
}

std::wstring FormatPercent(int percent, bool hide_percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    std::wostringstream oss;
    oss << percent;
    if (!hide_percent) oss << L'%';
    return oss.str();
}

} // namespace mm
