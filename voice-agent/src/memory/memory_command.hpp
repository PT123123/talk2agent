// src/memory/memory_command.hpp
#pragma once
#include "memory/memory_store.hpp"
#include <string>

namespace voice_agent {

// 是否是一条 /memory 命令（以 "memory " 或 "/memory " 开头）
bool is_memory_command(const std::string& text);

// 执行 memory 命令，返回面向播报/展示的纯文本。
// 支持：save <内容> / list [n] / forget <id或关键词> / clear / count
std::string run_memory_command(MemoryStore& store, const std::string& cmdline);

}  // namespace voice_agent