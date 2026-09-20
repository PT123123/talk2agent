// src/memory/memory_retriever.hpp
#pragma once
#include "core/types.hpp"
#include "memory/memory_store.hpp"
#include <memory>
#include <string>
#include <vector>

namespace voice_agent {

// ========== 记忆检索器 ==========
// 在 MemoryStore 双路径召回基础上，做语料级打分重排
// （查询单元覆盖率 + 关键词精确命中 + recency 衰减）。
class MemoryRetriever {
public:
    explicit MemoryRetriever(std::shared_ptr<MemoryStore> store);

    // 检索与 query 最相关的 k 条。匹配度存于 salience。
    std::vector<MemoryItem> retrieve(const std::string& query, int k) const;

    // 可用性过滤：当前时间是否在有效期内
    bool is_active(const MemoryItem& m) const;

private:
    std::shared_ptr<MemoryStore> store_;
};

}  // namespace voice_agent