// src/orchestrator/fast_response.hpp
#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "orchestrator/response_policy.hpp"

namespace voice_agent {

// ========== 抢答决策 ==========
enum class AckDecision {
    Silent,       // 不说，等着（预计很快）
    Acknowledge,  // 立即说一句短接手（"我看一下。"）
};

inline const char* ack_decision_to_string(AckDecision d) {
    return d == AckDecision::Silent ? "silent" : "acknowledge";
}

// ========== Fast Response Plan ==========
struct FastResponsePlan {
    AckDecision decision{AckDecision::Silent};
    std::string text;        // decision=Acknowledge 时非空
    int expected_ms{0};      // 预估的任务耗时
    std::string reason;      // 判定理由（写进 trace）
};

// ========== FastResponseLayer ==========
// 核心思想：复杂任务可以花时间，但前台对话不能因为后台工作而长时间"死寂"。
//
// 例：用户说"我最近真的有好多事情" —— 不应该干等深度模型 3 秒才第一次出声。
// 可以先"嗯。"，然后后台取记忆/近期对话/当前项目，最后再给完整回应。
//
// 关键约束（做错就是机器人味）：
//   1. 不是每句话都先嗯一下 —— 预计够快就闭嘴（Silent）
//   2. 阈值不是硬编码 —— 用实测耗时的滑动均值自适应，各环境（本地模型/在线 API）差异极大
//   3. 话术轮换 —— 每次都说"让我查一下"就是机器味的主要来源
class FastResponseLayer {
public:
    struct Config {
        bool enabled{true};
        // 预计耗时超过此值才抢答（ms）
        int ack_threshold_ms{700};
        // 没有历史样本时的默认预估（ms）—— 首轮没数据可依
        int default_estimate_ms{600};
        // 每个 task_type 最多说几句（防话痨）
        int max_acks_per_task{1};
        // EWMA 平滑系数（0~1，越小越看重历史）
        double ewma_alpha{0.3};
    };

    explicit FastResponseLayer(Config cfg = {}) : cfg_(cfg) {}

    // ---- 延迟自适应 ----
    // 记录一次实测耗时。task_type 为 "search" / "agent" / "llm" 等。
    void record_latency(const std::string& task_type, int ms);

    // 预测某类任务耗时（EWMA）。无样本时返回 default_estimate_ms。
    int estimate_latency_ms(const std::string& task_type) const;

    // ---- 抢答决策 ----
    // 根据 Policy 结论 + 预估耗时决定要不要立即说一句。
    // 注意：pure 决策，不产生副作用。
    FastResponsePlan plan(const ResponseDecision& decision,
                          const std::string& task_type,
                          const std::string& user_text) const;

    // ---- 轮次内的说话次数闸门 ----
    // 同一轮里已经说过几句（跨 ack 累计），超限就闭嘴。
    int acks_in_turn() const;
    void begin_turn();          // 新轮次开始，计数归零
    void notify_spoke();        // 实际说了一句
    void on_interrupt();        // 打断后清零

private:
    Config cfg_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, double> latency_ema_;
    std::atomic<int> acks_in_turn_{0};
};

}  // namespace voice_agent
