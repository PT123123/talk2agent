// src/memory/memory_retriever.cpp
#include "memory_retriever.hpp"
#include <chrono>
#include <algorithm>

namespace voice_agent {

MemoryRetriever::MemoryRetriever(std::shared_ptr<MemoryStore> store) : store_(std::move(store)) {}

bool MemoryRetriever::is_active(const MemoryItem& m) const {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
    if (m.valid_from > 0 && now < m.valid_from) return false;
    if (m.valid_to > 0 && now > m.valid_to) return false;
    return true;
}

std::vector<MemoryItem> MemoryRetriever::retrieve(const std::string& query, int k) const {
    if (!store_) return {};
    // 多取一些再过滤有效期
    auto cand = store_->query(query, k * 3);
    std::vector<MemoryItem> out;
    out.reserve(cand.size());
    for (auto& m : cand) {
        if (is_active(m)) out.push_back(std::move(m));
    }
    if (static_cast<int>(out.size()) > k) out.resize(k);
    return out;
}

}  // namespace voice_agent