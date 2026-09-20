// src/memory/memory_store.hpp
#pragma once
#include "core/types.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace voice_agent {

// ========== 持久化记忆库（SQLite + FTS5）==========
// 负责耐用存储、去重合并、双路径召回（FTS5 拉丁语 + LIKE 兜底中文短词）。
// 线程安全：所有方法内部加锁。
class MemoryStore {
public:
    // db_path: 磁盘文件路径，或 ":memory:" 用于测试。
    explicit MemoryStore(std::string db_path);
    ~MemoryStore();

    MemoryStore(const MemoryStore&) = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    // 打开/建表。失败返回 false（is_open() 保持 false）。
    bool open();

    bool is_open() const { return db_ != nullptr; }
    const std::string& db_path() const { return path_; }

    // 保存一条记忆。内容完全相同时合并（更新 subject/type/tags/时间）。
    // tags 用逗号拼接存储。返回条目 id；失败返回 -1。
    int64_t save(const std::string& subject, const std::string& content,
                 const std::vector<std::string>& tags = {},
                 int64_t ttl_days = 0);

    // 按相关度召回最相关的 k 条（Results 按得分倒序，salience 承载得分）。
    std::vector<MemoryItem> query(const std::string& text, int k);

    // 列出最近 limit 条（offset 分页），按更新时间倒序。
    std::vector<MemoryItem> list(int limit, int offset = 0);

    // 按 id 删除；返回是否删除到条目。
    bool remove(int64_t id);
    void clear();
    int64_t count() const;

private:
    std::vector<MemoryItem> select_rows_(const std::string& sql,
                                         const std::vector<std::string>& params);
    bool exec_(const std::string& sql);

    mutable std::mutex mutex_;
    sqlite3* db_{nullptr};
    std::string path_;
};

}  // namespace voice_agent