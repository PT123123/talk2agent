// src/memory/memory_extractor.hpp
#pragma once
#include "core/types.hpp"
#include <string>
#include <vector>

namespace voice_agent {

// ========== 记忆抽取器 ==========
// 从一句用户话中判断是否值得写入长期记忆，并抽取结构化条目。
// 默认使用中英文启发式基线；可注入 LLM 做更强的事实抽取。
class MemoryExtractor {
public:
    MemoryExtractor() = default;

    // 可选的事件回调，替换内部启发式倾向（暂未暴露事件总线）
    // 直接从原始话术抽取候选记忆。
    std::vector<MemoryExtractResult> extract(const std::string& user_text) const;

    // 是否启用 LLM 结构化抽取
    void set_enable_llm(bool v) { enable_llm_ = v; }
    bool enable_llm() const { return enable_llm_; }

private:
    bool enable_llm_{true};  // mock LLM 不可用时会自动退化为启发式
};

}  // namespace voice_agent