// src/orchestrator/background_cache.hpp
#pragma once
#include <chrono>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace voice_agent {

// ========== 一条后台结果 ==========
struct CachedResult {
    std::string topic_key;
    std::string content;
    int64_t created_at_ms{0};
    double elapsed_ms{0.0};
    bool superseded{false};   // 产出时话题已换：不主动播报，但可被引用
    int hit_count{0};         // 被引用次数
};

// ========== BackgroundCache ==========
// 后台任务结果的落地处。
//
// R1 遗留的问题在这里闭环：结果写进去了却没有读取路径，
// 于是"刚才那个搜索结果是什么来着"永远答不上。
//
// 设计要点：
//   1. **可引用但默认不播报**。superseded 的结果照样能被后续轮次引用
//      （"你说的是不是这个"），只是 Agent 不会主动插话。
//   2. **有 TTL**。搜索结果过期了还拿来回答是错的。
//   3. **有上限**。长时间运行不能无限涨（长跑测试的内存红线）。
//   4. **按 topic 去重**。同一 topic 多次结果以最新为准。
class BackgroundCache {
public:
    struct Config {
        int max_entries{64};        // 最多留多少条
        int64_t ttl_sec{1800};      // 过期时间（秒）
    };

    explicit BackgroundCache(Config cfg = {}) : cfg_(cfg) {}

    // 存入/覆盖一条结果
    void put(std::string topic_key, std::string content,
             double elapsed_ms = 0.0, bool superseded = false);

    // 按 topic 精确取
    std::optional<CachedResult> get(const std::string& topic_key) const;

    // 关键词模糊取：给"刚才那个搜索结果"这类指代用。
    // 命中返回最新的一条。
    std::optional<CachedResult> find_by_keyword(const std::string& query) const;

    // 最近 n 条（新的在前）
    std::vector<CachedResult> recent(size_t n = 5) const;

    // 摘掉所有已过期的
    size_t expire();

    size_t size() const;
    void clear();

    // 总字符数（长跑测试观测内存增长用）
    size_t total_chars() const;

private:
    static int64_t now_ms();
    // 关键词切分：中文按 2-gram，英文按空格
    static std::vector<std::string> tokenize(const std::string& s);
    static bool contains_topic(const std::string& haystack,
                               const std::vector<std::string>& tokens);

    Config cfg_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, CachedResult> map_;
    std::list<std::string> lru_;   // topic_key 的访问顺序（旧的在前）
};

}  // namespace voice_agent
