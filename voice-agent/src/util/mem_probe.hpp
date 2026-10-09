// src/util/mem_probe.hpp
#pragma once
#include <cstdint>
#include <string>

namespace voice_agent {

// 一次内存采样的结果（单位：字节）。
//
// private_bytes 是本项目唯一可信的"占用"口径：
//   * WorkingSet 里混着系统字体库等所有进程共享页，Windows 11 上光初始化一次
//     字体就虚增几十 MB，用它判断"这个模型吃了多少"会完全失真。
//   * PrivateUsage 只统计本进程独占提交的页，模型权重 / KV cache / 中间张量
//     都落在里面。
struct MemSample {
    std::uint64_t private_bytes = 0;   // 本进程私有提交（任务管理器"内存"的分母口径）
    std::uint64_t working_set = 0;     // 工作集（含共享页，仅作参考）
    std::uint64_t peak_working_set = 0;
    std::uint64_t sys_total = 0;       // 物理内存总量
    std::uint64_t sys_available = 0;   // 物理内存可用（不含 standby 的粗略值）
    bool valid = false;                // false = 采样失败（平台不支持/权限不足）
};

// 采样当前进程 + 系统内存。非 Windows 或 API 失败时返回 valid=false。
MemSample mem_probe();

// 便捷格式化：字节 → "1.2 GB" / "345 MB" / "12 KB"。
std::string format_bytes(std::uint64_t bytes);

}  // namespace voice_agent