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
#include <set>
#include <utility>

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

struct NetworkCandidate {
    ULONGLONG luid = 0;
    ULONGLONG in_octets = 0;
    ULONGLONG out_octets = 0;
    bool hardware = false;
};

static bool IsUsableNetworkRow(const MIB_IF_ROW2& row) {
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) return false;
    if (row.Type == IF_TYPE_TUNNEL) return false;
    if (row.OperStatus != IfOperStatusUp) return false;
    if (row.InterfaceAndOperStatusFlags.FilterInterface) return false;
    return row.InterfaceLuid.Value != 0;
}

static void AppendUniqueRouteInterface(std::vector<ULONGLONG>& result,
                                       ULONGLONG luid) {
    if (luid == 0) return;
    for (const ULONGLONG existing : result) {
        if (existing == luid) return;
    }
    result.push_back(luid);
}

static void AppendDefaultRouteInterfaces(ADDRESS_FAMILY family,
                                         std::vector<ULONGLONG>& result) {
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (GetIpForwardTable2(family, &table) != NO_ERROR || table == nullptr) {
        if (table) FreeMibTable(table);
        return;
    }

    ULONG best_metric = (std::numeric_limits<ULONG>::max)();
    std::vector<ULONGLONG> family_routes;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2& row = table->Table[i];
        if (row.DestinationPrefix.PrefixLength != 0 ||
            row.InterfaceLuid.Value == 0) {
            continue;
        }
        if (row.Metric < best_metric) {
            best_metric = row.Metric;
            family_routes.clear();
        }
        if (row.Metric == best_metric) {
            AppendUniqueRouteInterface(family_routes,
                                       row.InterfaceLuid.Value);
        }
    }
    for (const ULONGLONG luid : family_routes) {
        AppendUniqueRouteInterface(result, luid);
    }
    FreeMibTable(table);
}

static std::vector<ULONGLONG> GetDefaultRouteInterfaces() {
    std::vector<ULONGLONG> result;
    AppendDefaultRouteInterfaces(AF_INET, result);
    AppendDefaultRouteInterfaces(AF_INET6, result);
    return result;
}

void Monitor::SetNetworkSelection(const std::wstring& selection) {
    const std::wstring normalized = selection.empty()
        ? std::wstring(kNetworkSelectionAuto) : selection;
    if (network_selection_ == normalized) return;

    network_selection_ = normalized;
    // 统计来源改变后，旧接口的计数不能参与新接口的差分，
    // 否则第一次刷新可能显示一个虚假的超大速率。
    has_network_baseline_ = false;
    previous_network_counters_.clear();
    auto_active_luid_ = 0;
    auto_pending_luid_ = 0;
    auto_pending_samples_ = 0;
    network_source_changed_ = false;
    prev_in_ = 0;
    prev_out_ = 0;
    acc_up_ = 0;
    acc_down_ = 0;
}

std::vector<NetworkInterfaceInfo> Monitor::EnumerateNetworkInterfaces() const {
    std::vector<NetworkInterfaceInfo> result;
    PMIB_IF_TABLE2 table = nullptr;
    const auto status = GetIfTable2(&table);
    if (status != NO_ERROR || table == nullptr) {
        if (table) FreeMibTable(table);
        return result;
    }

    std::set<std::wstring> seen;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (!IsUsableNetworkRow(row)) continue;

        const std::wstring id = std::to_wstring(row.InterfaceLuid.Value);
        if (!seen.insert(id).second) continue;

        std::wstring name = row.Alias;
        if (name.empty()) name = row.Description;
        if (name.empty()) name = L"接口 " + id;
        name += row.InterfaceAndOperStatusFlags.HardwareInterface
            ? L"（物理）" : L"（VPN/虚拟）";

        result.push_back({id, name,
                          row.InterfaceAndOperStatusFlags.HardwareInterface != 0});
    }
    FreeMibTable(table);
    return result;
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

    std::vector<NetworkCandidate> candidates;
    std::vector<NetworkCounter> current_counters;
    candidates.reserve(table->NumEntries);
    current_counters.reserve(table->NumEntries);
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (!IsUsableNetworkRow(row)) continue;

        const ULONGLONG luid = row.InterfaceLuid.Value;
        candidates.push_back({luid, row.InOctets, row.OutOctets,
                              row.InterfaceAndOperStatusFlags.HardwareInterface != 0});
        current_counters.push_back({luid, row.InOctets, row.OutOctets});
    }

    auto find_candidate = [&](ULONGLONG luid) -> const NetworkCandidate* {
        for (const auto& candidate : candidates) {
            if (candidate.luid == luid) return &candidate;
        }
        return nullptr;
    };
    auto previous_delta = [&](const NetworkCandidate& candidate) -> ULONGLONG {
        for (const auto& previous : previous_network_counters_) {
            if (previous.luid != candidate.luid) continue;
            if (candidate.in_octets < previous.in_octets ||
                candidate.out_octets < previous.out_octets) {
                return 0;
            }
            const ULONGLONG delta_in = candidate.in_octets - previous.in_octets;
            const ULONGLONG delta_out = candidate.out_octets - previous.out_octets;
            const ULONGLONG max_value = (std::numeric_limits<ULONGLONG>::max)();
            return delta_in > max_value - delta_out
                ? max_value : delta_in + delta_out;
        }
        return 0;
    };
    auto choose_busy_candidate = [&](bool hardware_only) -> ULONGLONG {
        const NetworkCandidate* best = nullptr;
        ULONGLONG best_delta = 0;
        for (const auto& candidate : candidates) {
            if (hardware_only && !candidate.hardware) continue;
            const ULONGLONG delta = previous_delta(candidate);
            if (!best || delta > best_delta) {
                best = &candidate;
                best_delta = delta;
            }
        }
        return best ? best->luid : 0;
    };

    if (network_selection_ == kNetworkSelectionAuto) {
        // 自动模式优先跟随 IPv4/IPv6 的默认路由。VPN 接管默认路由后，
        // 这里会得到 VPN 的 LUID；没有可用默认路由时再按活动流量回退。
        const auto route_interfaces = GetDefaultRouteInterfaces();
        ULONGLONG desired_luid = 0;
        ULONGLONG desired_delta = 0;
        for (const ULONGLONG route_luid : route_interfaces) {
            const NetworkCandidate* candidate = find_candidate(route_luid);
            if (!candidate) continue;
            const ULONGLONG delta = previous_delta(*candidate);
            if (desired_luid == 0 || delta > desired_delta) {
                desired_luid = candidate->luid;
                desired_delta = delta;
            }
        }
        if (desired_luid == 0) {
            desired_luid = choose_busy_candidate(true);
            if (desired_luid == 0) desired_luid = choose_busy_candidate(false);
        }

        constexpr int kAutoSwitchSamples = 3;
        const bool active_present = find_candidate(auto_active_luid_) != nullptr;
        if (auto_active_luid_ == 0) {
            auto_active_luid_ = desired_luid;
            auto_pending_luid_ = 0;
            auto_pending_samples_ = 0;
            network_source_changed_ = true;
        } else if (desired_luid == auto_active_luid_) {
            auto_pending_luid_ = 0;
            auto_pending_samples_ = 0;
        } else if (!active_present && desired_luid != 0) {
            // 当前接口已经消失，不能等待稳定窗口，否则 VPN 断开后会长时间显示旧值。
            auto_active_luid_ = desired_luid;
            auto_pending_luid_ = 0;
            auto_pending_samples_ = 0;
            network_source_changed_ = true;
        } else if (desired_luid == 0) {
            auto_pending_luid_ = 0;
            auto_pending_samples_ = 0;
            if (!active_present) {
                auto_active_luid_ = 0;
                network_source_changed_ = true;
            }
        } else if (desired_luid != 0) {
            if (auto_pending_luid_ != desired_luid) {
                auto_pending_luid_ = desired_luid;
                auto_pending_samples_ = 1;
            } else if (++auto_pending_samples_ >= kAutoSwitchSamples) {
                auto_active_luid_ = desired_luid;
                auto_pending_luid_ = 0;
                auto_pending_samples_ = 0;
                network_source_changed_ = true;
            }
        }

        const NetworkCandidate* selected = find_candidate(auto_active_luid_);
        if (selected) {
            out_total_in = selected->in_octets;
            out_total_out = selected->out_octets;
        }
    } else if (network_selection_ == kNetworkSelectionAll) {
        // 用户明确要求全部接口时才累加，保留其可能重复计数的语义。
        for (const auto& candidate : candidates) {
            out_total_in += candidate.in_octets;
            out_total_out += candidate.out_octets;
        }
    } else {
        for (const auto& candidate : candidates) {
            if (std::to_wstring(candidate.luid) != network_selection_) continue;
            out_total_in = candidate.in_octets;
            out_total_out = candidate.out_octets;
            break;
        }
    }

    previous_network_counters_ = std::move(current_counters);
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
    const bool network_source_changed = network_source_changed_;
    network_source_changed_ = false;
    if (!network_ok) {
        has_network_baseline_ = false;
    } else {
        // 差分。注意计数器可能在系统睡眠/网卡重置后回绕或变小，做保护。
        if (!network_source_changed && has_network_baseline_ && cur_in >= prev_in_) {
            delta_in = cur_in - prev_in_;
            have_in_delta = true;
            m.net_down_bytes_per_sec = BytesPerSecond(delta_in, elapsed_ms);
        }
        if (!network_source_changed && has_network_baseline_ && cur_out >= prev_out_) {
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
