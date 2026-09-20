// src/memory/memory_manager.hpp
#pragma once
#include "core/types.hpp"
#include "memory/memory_store.hpp"
#include "memory/memory_extractor.hpp"
#include "memory/memory_retriever.hpp"
#include <memory>
#include <string>
#include <vector>

namespace voice_agent {

// ========== 记忆管理器（高层门面）==========
// 串起 抽取 → 存储 → 检索 / 命令 的完整流程。
class MemoryManager {
public:
    explicit MemoryManager(std::string db_path);

    bool open();
    bool is_open() const { return store_ && store_->is_open(); }

    // 对话收尾：抽取用户话术中的可记忆事实并落库。返回实际写入条数。
    int ingest(const std::string& user_text, const std::string& assistant_text = {});

    // 召回与用户输入最相关的记忆（供补充到 agent system prompt）。
    std::vector<MemoryItem> recall(const std::string& context, int k = 4);

    // 直接查询 / 列出 / 删除。
    std::vector<MemoryItem> query(const std::string& text, int k = 5);
    std::vector<MemoryItem> list(int limit, int offset = 0);
    bool forget(int64_t id);
    void clear();
    int64_t count() const;

    // 底层组件访问
    std::shared_ptr<MemoryStore> store() const { return store_; }

    // /memory 命令文本入口
    std::string run_command(const std::string& cmdline);

private:
    std::shared_ptr<MemoryStore> store_;
    std::shared_ptr<MemoryRetriever> retriever_;
    MemoryExtractor extractor_;
};

}  // namespace voice_agent