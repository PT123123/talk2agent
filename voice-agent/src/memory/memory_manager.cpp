// src/memory/memory_manager.cpp
#include "memory_manager.hpp"
#include "memory_command.hpp"
#include "util/log.hpp"

namespace voice_agent {

MemoryManager::MemoryManager(std::string db_path)
    : store_(std::make_shared<MemoryStore>(std::move(db_path))),
      retriever_(std::make_shared<MemoryRetriever>(store_)) {}

bool MemoryManager::open() {
    if (!store_->open()) return false;
    LOG_INFO("MemoryManager ready at '{}'", store_->db_path());
    return true;
}

int MemoryManager::ingest(const std::string& user_text, const std::string&) {
    if (!store_) return 0;
    auto candidates = extractor_.extract(user_text);
    int written = 0;
    for (auto& c : candidates) {
        if (!c.worth_saving) continue;
        if (store_->save(c.subject, c.content, {}, c.ttl_days) >= 0) {
            ++written;
            LOG_INFO("Memory: saved '{}' ({})", c.content, c.type);
        }
    }
    return written;
}

std::vector<MemoryItem> MemoryManager::recall(const std::string& context, int k) {
    if (!retriever_) return {};
    return retriever_->retrieve(context, k);
}

std::vector<MemoryItem> MemoryManager::query(const std::string& text, int k) {
    return store_ ? store_->query(text, k) : std::vector<MemoryItem>{};
}

std::vector<MemoryItem> MemoryManager::list(int limit, int offset) {
    return store_ ? store_->list(limit, offset) : std::vector<MemoryItem>{};
}

bool MemoryManager::forget(int64_t id) { return store_ && store_->remove(id); }
void MemoryManager::clear() { if (store_) store_->clear(); }
int64_t MemoryManager::count() const { return store_ ? store_->count() : 0; }

std::string MemoryManager::run_command(const std::string& cmdline) {
    if (!store_) return "记忆系统不可用。";
    return run_memory_command(*store_, cmdline);
}

}  // namespace voice_agent