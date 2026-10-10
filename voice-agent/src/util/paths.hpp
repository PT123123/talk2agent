// src/util/paths.hpp
#pragma once
#include <string>

namespace voice_agent {

// 模型根目录：%LOCALAPPDATA%\VoiceAgent\models（用户级存储，清工程/重装不丢模型）。
// 环境变量缺失时回退 %USERPROFILE%\AppData\Local；再缺失则退回旧相对路径 "models"。
std::string models_root();

// 模型路径重映射（配置里的旧写法 → 用户目录）：
//   空 → 空；绝对路径（C:/... 或 /...）→ 原样（自定义位置优先）；
//   "models/..." 前缀 → models_root() + 去前缀部分；其它相对路径 → 原样（按 CWD）。
std::string remap_model_path(const std::string& path);

// remap 的逆运算：把 models_root() 下的绝对路径还原成 "models/..." 相对写法，
// 用于把路径写回 configs/agent.yaml（配置文件不落机器相关绝对路径）。
std::string unmap_model_path(const std::string& path);

// 日志根目录：%LOCALAPPDATA%\VoiceAgent\logs（与 models 同级的用户级存储）。
// 环境变量缺失时的回退规则与 models_root() 一致；再缺失则退回 "logs"（按 CWD）。
std::string logs_root();

// 一次性迁移：旧版模型放在 <CWD>/models 或 <exe目录>/models，
// 新根目录不存在且旧目录存在时整体 rename 到 models_root()。
// rename 失败（如跨盘）不阻塞启动，仅记日志提示手动搬移。
void migrate_legacy_models();

}  // namespace voice_agent
