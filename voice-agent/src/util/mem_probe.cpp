// src/util/mem_probe.cpp
#include "util/mem_probe.hpp"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
// psapi.h 提供 GetProcessMemoryInfo；PROCESS_MEMORY_COUNTERS_EX 里的
// PrivateUsage 比 WorkingSetSize 更能反映"本进程真实独占了多少内存"。
#include <psapi.h>
#endif

namespace voice_agent {

MemSample mem_probe() {
    MemSample s;
#ifdef _WIN32
    // 进程：PROCESS_MEMORY_COUNTERS_EX 才能拿到 PrivateUsage，
    // PROCESS_MEMORY_COUNTERS（不带 _EX）结构更小，没有这个字段。
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    // K32GetProcessMemoryInfo 是 Windows 7+ 的 kernel32 转发入口，
    // 不像 GetProcessMemoryInfo 那样需要额外链接 psapi.lib。
    // 注意签名只接受非 _EX 版本，但结构体布局是前缀兼容的
    // （_EX 就是把基础结构末尾追加 PrivateCommit 等字段），
    // 所以 reinterpret_cast 传 _EX 指针是官方文档给出的写法。
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc),
                                sizeof(pmc))) {
        s.private_bytes = static_cast<std::uint64_t>(pmc.PrivateUsage);
        s.working_set = static_cast<std::uint64_t>(pmc.WorkingSetSize);
        s.peak_working_set = static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
        s.valid = true;
    }

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.sys_total = static_cast<std::uint64_t>(ms.ullTotalPhys);
        s.sys_available = static_cast<std::uint64_t>(ms.ullAvailPhys);
        // 系统内存读到了就算这次采样有效（哪怕进程计数失败）。
        s.valid = s.valid || (s.sys_total > 0);
    }
#endif
    return s;
}

std::string format_bytes(std::uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024) {
        std::snprintf(buf, sizeof(buf), "%.0f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.0f KB",
                      static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return buf;
}

}  // namespace voice_agent