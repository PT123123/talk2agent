// src/core/working_context.cpp
#include "core/working_context.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace voice_agent {

// ========== WorkingContext ==========

int64_t WorkingContext::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void WorkingContext::set_topic(std::string topic) {
    std::lock_guard<std::mutex> lock(mutex_);
    topic_ = std::move(topic);
}

void WorkingContext::set(const std::string& key, std::string value, int ttl_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    Item it;
    it.value = std::move(value);
    it.expire_at_ms = ttl_sec > 0 ? now_ms() + ttl_sec * 1000LL : 0;
    items_[key] = std::move(it);
}

std::optional<std::string> WorkingContext::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = items_.find(key);
    if (it == items_.end()) return std::nullopt;
    if (it->second.expire_at_ms > 0 && now_ms() > it->second.expire_at_ms) {
        return std::nullopt;
    }
    return it->second.value;
}

bool WorkingContext::has(const std::string& key) const {
    return get(key).has_value();
}

void WorkingContext::unset(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.erase(key);
}

void WorkingContext::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    entities_.clear();
    topic_.clear();
}

void WorkingContext::push_entity(std::string entity) {
    if (entity.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    // 去重后前插，保持最近优先
    entities_.erase(std::remove(entities_.begin(), entities_.end(), entity),
                    entities_.end());
    entities_.insert(entities_.begin(), std::move(entity));
    if (entities_.size() > 12) entities_.resize(12);
}

std::optional<std::string> WorkingContext::latest_entity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entities_.empty()) return std::nullopt;
    return entities_.front();
}

void WorkingContext::set_active_project(std::string p) {
    set("active_project", std::move(p));
}

void WorkingContext::on_topic_changed(std::string new_topic) {
    std::lock_guard<std::mutex> lock(mutex_);
    topic_ = std::move(new_topic);
    // 换话题 => 临时项全部失效（长期有效的项应显式给 ttl 或写入 Memory）
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->second.expire_at_ms == 0) {
            it = items_.erase(it);
        } else {
            ++it;
        }
    }
    entities_.clear();
}

size_t WorkingContext::expire() {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now = now_ms();
    size_t n = 0;
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->second.expire_at_ms > 0 && now > it->second.expire_at_ms) {
            it = items_.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

std::map<std::string, std::string> WorkingContext::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, std::string> out;
    const int64_t now = now_ms();
    for (auto& [k, v] : items_) {
        if (v.expire_at_ms > 0 && now > v.expire_at_ms) continue;
        out[k] = v.value;
    }
    return out;
}

WorkingContext::Snapshot WorkingContext::snapshot_with_entity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot s;
    const int64_t now = now_ms();
    for (auto& [k, v] : items_) {
        if (v.expire_at_ms > 0 && now > v.expire_at_ms) continue;
        s.items[k] = v.value;
    }
    if (!entities_.empty()) s.latest_entity = entities_.front();
    return s;
}

// ========== ContextManager ==========

namespace {
// 按预算裁剪字符串，尾部省略
std::string clip(const std::string& s, int max_chars) {
    if (max_chars <= 0) return {};
    if (static_cast<int>(s.size()) <= max_chars) return s;
    if (max_chars <= 16) return s.substr(0, max_chars);
    return s.substr(0, max_chars - 3) + "...";
}

std::string join_clipped(const std::vector<std::string>& items, int max_chars,
                         const char* sep = "\n") {
    std::string out;
    for (const auto& s : items) {
        if (max_chars > 0 && static_cast<int>(out.size() + s.size()) > max_chars) break;
        if (!out.empty()) out += sep;
        out += s;
    }
    return clip(out, max_chars);
}
}  // namespace

void ContextManager::add_turn(std::string user, std::string assistant) {
    std::lock_guard<std::mutex> lock(mutex_);
    ConversationTurn t;
    t.user = std::move(user);
    t.assistant = std::move(assistant);
    t.ts_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    turns_.push_back(std::move(t));

    // ---- P7 滚动摘要 ----
    // 超出窗口的轮次不能简单丢弃：模型会"忘记"前面聊过什么，
    // 长对话里表现为反复问同一件事。所以把它们压缩成摘要保留。
    if (turns_.size() > static_cast<size_t>(cfg_.recent_turns) + summarize_every_) {
        summarize_old_();
    }

    if (static_cast<int>(turns_.size()) > cfg_.max_session_turns) {
        // 硬上限：超出就丢最旧的完整轮次（摘要仍在）
        turns_.erase(turns_.begin(),
                     turns_.begin() + (turns_.size() - cfg_.max_session_turns));
    }
}

void ContextManager::summarize_old_() {
    // 把"窗口之外、且尚未摘要"的轮次压进 older_summary_
    const size_t keep = static_cast<size_t>(cfg_.recent_turns);
    const size_t old_count = turns_.size() - keep;
    if (old_count <= summarized_count_) return;

    // 增量摘要：只处理新超出的那几轮
    std::string added;
    for (size_t i = summarized_count_; i < old_count; ++i) {
        const auto& t = turns_[i];
        if (!t.user.empty()) added += "问:" + t.user + " ";
        if (!t.assistant.empty()) {
            // 回答只取前 60 字：摘要要的是"聊过什么"，不是全文
            std::string a = t.assistant.substr(0, 60);
            added += "答:" + a + " ";
        }
    }
    summarized_count_ = old_count;
    if (added.empty()) return;

    // 与旧摘要合并，超长则裁掉最早的部分（保留最近的更符合"当前话题"）
    older_summary_ = older_summary_.empty()
                         ? added
                         : older_summary_ + " | " + added;
    if (static_cast<int>(older_summary_.size()) > static_cast<int>(summary_max_chars_)) {
        const size_t overflow = older_summary_.size() - summary_max_chars_;
        older_summary_ = older_summary_.substr(overflow);
    }
}

void ContextManager::set_summary_policy(size_t every_n, size_t max_chars) {
    std::lock_guard<std::mutex> lock(mutex_);
    summarize_every_ = every_n ? every_n : 8;
    summary_max_chars_ = max_chars ? max_chars : 600;
}

std::string ContextManager::older_summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return older_summary_;
}

size_t ContextManager::summarized_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return summarized_count_;
}

void ContextManager::add_tool_result(std::string content) {
    if (content.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    results_.push_back(ContextEntry{"tool", std::move(content)});
}

void ContextManager::add_background_result(std::string content) {
    if (content.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    results_.push_back(ContextEntry{"background", std::move(content)});
}

void ContextManager::clear_results() {
    std::lock_guard<std::mutex> lock(mutex_);
    results_.clear();
}

void ContextManager::set_relevant_memory(std::vector<std::string> memories) {
    std::lock_guard<std::mutex> lock(mutex_);
    memory_ = std::move(memories);
}

void ContextManager::set_task_state_summary(std::string s) {
    std::lock_guard<std::mutex> lock(mutex_);
    task_state_summary_ = std::move(s);
}

std::string ContextManager::build_context() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> blocks;
    auto remaining = cfg_.total_char_budget;

    auto push = [&](const std::string& header, const std::string& body) {
        if (body.empty()) return;
        std::string b = header.empty() ? body : (header + "\n" + body);
        if (static_cast<int>(b.size()) > remaining) {
            b = clip(b, std::max(0, remaining));
        }
        if (b.empty()) return;
        blocks.push_back(b);
        remaining -= static_cast<int>(b.size());
    };

    // P1 当前话题（最高优先，永不被裁）
    if (!working_.topic().empty()) push("[当前话题]", working_.topic());

    // P2 任务状态
    push("[进行中的任务]", task_state_summary_);

    // P3 最近几轮（按时间正序，最新的在最后 —— 更靠近生成位置）
    {
        const int n = std::min<int>(cfg_.recent_turns,
                                    static_cast<int>(turns_.size()));
        if (n > 0) {
            std::vector<std::string> lines;
            for (size_t i = turns_.size() - n; i < turns_.size(); ++i) {
                std::string line = "用户: " + turns_[i].user;
                if (!turns_[i].assistant.empty()) {
                    line += "\n助手: " + turns_[i].assistant;
                }
                lines.push_back(std::move(line));
            }
            push("[最近对话]", join_clipped(lines, cfg_.total_char_budget / 2, "\n\n"));
        }
    }

    // P4 工作上下文
    {
        auto snap = working_.snapshot_with_entity();
        if (!snap.items.empty() || !snap.latest_entity.empty()) {
            std::vector<std::string> lines;
            for (auto& [k, v] : snap.items) lines.push_back(k + ": " + v);
            if (!snap.latest_entity.empty()) {
                lines.push_back("recent_entity: " + snap.latest_entity);
            }
            push("[工作上下文]", join_clipped(lines, cfg_.working_context_max));
        }
    }

    // P5 召回记忆
    push("[相关记忆]", join_clipped(memory_, cfg_.memory_max, "\n"));

    // P6 工具/后台结果（多来源并存，按来源分组）
    if (!results_.empty()) {
        std::vector<std::string> lines;
        std::string cur_source;
        std::string buf;
        auto flush = [&] {
            if (!buf.empty()) {
                lines.push_back("[" + cur_source + "] " + buf);
                buf.clear();
            }
        };
        for (const auto& e : results_) {
            if (e.source != cur_source) {
                flush();
                cur_source = e.source;
            }
            buf += e.content;
            if (buf.size() > 1 && buf.back() != '\n') buf += "\n";
        }
        flush();
        push("[工具与后台结果]", join_clipped(lines, cfg_.tool_results_max, "\n"));
    }

    // P7 更早历史（滚动摘要，不是干巴巴一句"已省略 N 轮"）
    if (!older_summary_.empty()) {
        push("[更早的对话摘要]", older_summary_);
    } else if (turns_.size() > static_cast<size_t>(cfg_.recent_turns)) {
        // 注意：直接读 turns_，不要调 has_older_history() ——
        // 那个方法内部会 lock(mutex_)，而本函数已持有它。
        // std::mutex 不可重入，自死锁在 Release 下表现为 0xC0000409 栈保护误报，
        // Debug 下是 _XDEBUG_ASSERT，很难第一时间联想到死锁。
        size_t older = turns_.size() - static_cast<size_t>(cfg_.recent_turns);
        push("[更早的历史]",
             "（本次会话中还有 " + std::to_string(older) + " 轮更早的对话已省略）");
    }

    std::string out;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out += "\n\n";
        out += blocks[i];
    }
    return out;
}

size_t ContextManager::turn_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return turns_.size();
}

bool ContextManager::has_older_history() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return turns_.size() > static_cast<size_t>(cfg_.recent_turns);
}

void ContextManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    turns_.clear();
    memory_.clear();
    results_.clear();
    task_state_summary_.clear();
    older_summary_.clear();
    summarized_count_ = 0;
    working_.clear();
}

}  // namespace voice_agent
