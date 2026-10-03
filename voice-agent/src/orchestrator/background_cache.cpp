// src/orchestrator/background_cache.cpp
#include "background_cache.hpp"

#include <algorithm>
#include <cctype>

namespace voice_agent {

int64_t BackgroundCache::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void BackgroundCache::put(std::string topic_key, std::string content,
                          double elapsed_ms, bool superseded) {
    if (topic_key.empty() || content.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);

    CachedResult r;
    r.topic_key = topic_key;
    r.content = std::move(content);
    r.created_at_ms = now_ms();
    r.elapsed_ms = elapsed_ms;
    r.superseded = superseded;

    // 同 topic 覆盖：保留原有 hit_count（同一条结果的引用次数不该被重置）
    auto it = map_.find(topic_key);
    if (it != map_.end()) {
        r.hit_count = it->second.hit_count;
        lru_.remove(topic_key);
    }
    map_[topic_key] = std::move(r);
    lru_.push_back(topic_key);

    // 容量上限：淘汰最旧的
    while (static_cast<int>(lru_.size()) > cfg_.max_entries) {
        const std::string oldest = lru_.front();
        lru_.pop_front();
        map_.erase(oldest);
    }
}

std::optional<CachedResult> BackgroundCache::get(const std::string& topic_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(topic_key);
    if (it == map_.end()) return std::nullopt;
    if (now_ms() - it->second.created_at_ms > cfg_.ttl_sec * 1000LL) {
        return std::nullopt;   // 过期即视为不存在：宁可没有也不要给错的
    }
    return it->second;
}

std::vector<std::string> BackgroundCache::tokenize(const std::string& s) {
    // 混合分词：ASCII 串按词切，CJK 按 2-gram 切。
    // 之所以必须切：ASR 文本里中文和英文/数字混排（"刚才那个 Qwen3 的搜索结果"），
    // 整串当一个 token 的话，"Qwen3" 与 topic 里的 "Qwen3" 永远匹配不上。
    std::vector<std::string> out;
    std::string ascii;
    // 收集 CJK 字符序列（稍后做 bigram）
    std::string cjk;

    auto flush_ascii = [&] {
        if (ascii.size() >= 2) out.push_back(ascii);
        ascii.clear();
    };
    auto flush_cjk = [&] {
        if (cjk.size() == 1) {
            out.push_back(cjk);            // 单字也要（"钱"、"车"）
        } else {
            for (size_t i = 0; i + 1 < cjk.size(); ++i) {
                out.push_back(cjk.substr(i, 2));
            }
        }
        cjk.clear();
    };

    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            // 取完整 UTF-8 码点
            size_t len = 1;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            if (i + len > s.size()) len = 1;
            flush_ascii();
            cjk.append(s, i, len);
            i += len;
        } else if (std::isalnum(c)) {
            flush_cjk();
            ascii.push_back(static_cast<char>(c));
            ++i;
        } else {
            flush_ascii();
            flush_cjk();
            ++i;
        }
    }
    flush_ascii();
    flush_cjk();
    return out;
}

bool BackgroundCache::contains_topic(const std::string& haystack,
                                     const std::vector<std::string>& tokens) {
    if (tokens.empty()) return false;

    // 加权命中：长 token（"Qwen3"、"qwen3-tts"）比 2-gram 更有信息量，
    // 但"命中过半"对 bigram 集合太严（用户一句话里的多数 bigram 与 topic 无关）。
    // 规则：命中任意 >=3 字符的 token 即算相关；否则要求 bigram 命中 >= 1/3。
    size_t long_hit = 0, short_total = 0, short_hit = 0;
    for (const auto& t : tokens) {
        if (t.size() >= 3) {
            if (haystack.find(t) != std::string::npos) ++long_hit;
        } else {
            ++short_total;
            if (haystack.find(t) != std::string::npos) ++short_hit;
        }
    }
    if (long_hit > 0) return true;
    if (short_total == 0) return false;
    return short_hit * 3 >= short_total;
}

std::optional<CachedResult> BackgroundCache::find_by_keyword(
    const std::string& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto tokens = tokenize(query);
    if (tokens.empty()) return std::nullopt;

    const int64_t ttl_ms = cfg_.ttl_sec * 1000;
    std::optional<CachedResult> best;

    for (const auto& [key, r] : map_) {
        if (now_ms() - r.created_at_ms > ttl_ms) continue;
        if (!contains_topic(key + " " + r.content, tokens)) continue;
        // 取最新的命中项
        if (!best.has_value() || r.created_at_ms > best->created_at_ms) {
            best = r;
        }
    }
    return best;
}

std::vector<CachedResult> BackgroundCache::recent(size_t n) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CachedResult> out;
    const int64_t ttl_ms = cfg_.ttl_sec * 1000;

    // lru_ 的尾部是最新的
    for (auto it = lru_.rbegin(); it != lru_.rend() && out.size() < n; ++it) {
        auto f = map_.find(*it);
        if (f == map_.end()) continue;
        if (now_ms() - f->second.created_at_ms > ttl_ms) continue;
        out.push_back(f->second);
    }
    return out;
}

size_t BackgroundCache::expire() {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t ttl_ms = cfg_.ttl_sec * 1000;
    size_t n = 0;
    for (auto it = map_.begin(); it != map_.end();) {
        if (now_ms() - it->second.created_at_ms > ttl_ms) {
            lru_.remove(it->first);
            it = map_.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

size_t BackgroundCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

void BackgroundCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    map_.clear();
    lru_.clear();
}

size_t BackgroundCache::total_chars() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& [k, r] : map_) n += k.size() + r.content.size();
    return n;
}

}  // namespace voice_agent
