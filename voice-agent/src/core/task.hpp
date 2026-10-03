// src/core/task.hpp
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/cancel_token.hpp"
#include "core/event_bus.hpp"
#include "core/types.hpp"

namespace voice_agent {

// ========== 任务优先级 ==========
// Foreground：用户正在等它的结果（当前轮回答、必要的工具调用）。
//             任何情况下都必须最快被调度，且享有 worker 保留位。
// Background：用户没在等（换话题前的搜索、预取、记忆整理）。
//             可以被前台任务插队、被 supersede 而不终止。
// Speculative：纯投机（边听边预取）。用户改口后结果直接丢弃或入 cache，
//              绝不能抢话播报。
enum class TaskPriority {
    Foreground,
    Background,
    Speculative
};

inline const char* task_priority_to_string(TaskPriority p) {
    switch (p) {
        case TaskPriority::Foreground:   return "Foreground";
        case TaskPriority::Background:   return "Background";
        case TaskPriority::Speculative: return "Speculative";
    }
    return "Unknown";
}

// ========== 任务状态 ==========
enum class TaskState {
    Queued,
    Running,
    Paused,
    Completed,
    Cancelled,
    Superseded,   // 被同 topic 的新前台任务取代：不终止，跑完只入 cache
    Failed
};

inline const char* task_state_to_string(TaskState s) {
    switch (s) {
        case TaskState::Queued:     return "Queued";
        case TaskState::Running:    return "Running";
        case TaskState::Paused:     return "Paused";
        case TaskState::Completed:  return "Completed";
        case TaskState::Cancelled:  return "Cancelled";
        case TaskState::Superseded: return "Superseded";
        case TaskState::Failed:     return "Failed";
    }
    return "Unknown";
}

// ========== 被打断时的处置策略 ==========
// 关键设计：用户打断后不是所有任务都一刀切 cancel。
enum class TaskPolicy {
    CancelOnInterrupt,   // 立刻取消（前台回答、正在播报的内容）
    PauseOnInterrupt,    // 暂停，可 resume（半成品可续的工作）
    ContinueOnInterrupt, // 继续跑完，结果入 cache 不播报
    SupersedeOnNewTopic  // 换话题时被新前台任务 supersede
};

inline const char* task_policy_to_string(TaskPolicy p) {
    switch (p) {
        case TaskPolicy::CancelOnInterrupt:   return "CancelOnInterrupt";
        case TaskPolicy::PauseOnInterrupt:    return "PauseOnInterrupt";
        case TaskPolicy::ContinueOnInterrupt: return "ContinueOnInterrupt";
        case TaskPolicy::SupersedeOnNewTopic: return "SupersedeOnNewTopic";
    }
    return "Unknown";
}

using TaskId = uint64_t;
inline constexpr TaskId kInvalidTaskId = 0;

// ========== 任务产出 ==========
struct TaskOutcome {
    bool ok{false};
    std::string value;         // 主体结果（文本/摘要/序列化数据）
    std::string error;         // 失败原因
    double elapsed_ms{0.0};

    // 该结果是否允许被播报。Superseded / ContinueOnInterrupt 的任务
    // 结果照常返回，但 should_speak=false —— 回到 context/cache，不抢话。
    bool should_speak{false};
};

// ========== 任务执行上下文（交给工作函数）============
class TaskContext {
public:
    TaskContext(TaskId id, std::string name, CancelToken::Ptr token,
                std::function<bool()> is_superseded = {})
        : id_(id), name_(std::move(name)), token_(std::move(token)),
          is_superseded_(std::move(is_superseded)) {}

    TaskId id() const { return id_; }
    const std::string& name() const { return name_; }
    CancelToken::Ptr token() const { return token_; }

    // 任务是否已被取消（工作函数应周期性检查）
    bool cancelled() const { return token_ && token_->cancelled(); }

    // 任务是否已被 supersede（不取消，但结果不该播报）。
    // 读的是 TaskManager 里的真实状态 —— 与 cancel() 走的是同一条通路，
    // 不能只靠 TaskContext 自己的标志位，否则两处状态会各说各话。
    bool superseded() const {
        if (superseded_.load(std::memory_order_acquire)) return true;
        return is_superseded_ && is_superseded_();
    }
    void mark_superseded() { superseded_.store(true, std::memory_order_release); }

    // 等待被取消/暂停解除；返回 false 表示应当提前退出。
    // 用法：长循环里 `if (!ctx.wait_or_cancel(50ms)) return cancelled_outcome;`
    bool wait_or_cancel(int timeout_ms);

    // 上报进度（0~100）。会发 TaskProgress 事件。
    void progress(int percent, const std::string& note = {});

private:
    TaskId id_;
    std::string name_;
    CancelToken::Ptr token_;
    std::function<bool()> is_superseded_;
    std::atomic<bool> superseded_{false};
};

// ========== 任务描述 ==========
struct TaskSpec {
    std::string name;                       // 人类可读名（trace 用）
    TaskPriority priority{TaskPriority::Background};
    TaskPolicy policy{TaskPolicy::ContinueOnInterrupt};

    // topic_key：语义分组键。同 key 的任务在用户换话题时会被 supersede。
    // 例："search:qwen3-tts"。空字符串表示不参与 supersede。
    std::string topic_key;

    int timeout_ms{0};                      // 0 = 不限
    uint64_t turn_id{0};                    // 发起该任务的对话轮次

    // 工作函数。返回 TaskOutcome。
    std::function<TaskOutcome(TaskContext&)> work;
};

// ========== 任务快照（只读观测，供 GUI / replay / 测试）============
struct TaskSnapshot {
    TaskId id{0};
    std::string name;
    std::string topic_key;
    TaskPriority priority{TaskPriority::Background};
    TaskPolicy policy{TaskPolicy::ContinueOnInterrupt};
    TaskState state{TaskState::Queued};
    uint64_t turn_id{0};
    int progress{0};
    double elapsed_ms{0.0};
    bool should_speak{false};
};

// ========== TaskManager ==========
// 固定 worker 池 + 优先级队列。前台任务享有"保留位"：
// 只要前台队列非空，就不会启动新的 Background/Speculative 任务，
// 因此一次慢的后台搜索不会把用户正在等的那一轮堵住。
class TaskManager {
public:
    using ResultSink = std::function<void(const TaskId&, const TaskOutcome&)>;

    explicit TaskManager(int workers = 3);
    ~TaskManager();

    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;

    // 启停 worker 池。Orchestrator 的 start/stop 会被反复调用
    // （GUI 的启停开关、模型切换会重建 Orchestrator），所以必须可重启。
    // 幂等：重复 start 无副作用，重复 stop 也无副作用。
    void start();
    void stop();
    bool running() const { return running_.load(); }

    // 提交任务，立即返回 task_id（非阻塞）。
    // 若 worker 池未启动，会自动 start（避免"忘记 start 就静默丢任务"）。
    TaskId submit(TaskSpec spec);

    // 结果回调：任务结束（成功/失败/被取消/被 supersede）时调用一次。
    // 用途：把后台结果塞进 cache / WorkingContext，而不是直接播报。
    void set_result_sink(ResultSink sink) { sink_ = std::move(sink); }

    // ---- 单任务控制 ----
    bool cancel(TaskId id);       // 立即取消
    bool pause(TaskId id);        // 暂停（若在队列中则不出队）
    bool resume(TaskId id);       // 恢复
    bool supersede(TaskId id);    // 标记被取代：不取消，跑完结果不播报

    // ---- 按策略批量处置（用户打断时调用）----
    // 遍历所有未结束任务，按各自 policy 决定 cancel / pause / continue。
    // 返回受影响的任务 id。
    std::vector<TaskId> apply_interrupt();

    // ---- 按 topic supersede（用户换话题时调用）----
    // 把 topic_key 匹配、且非 Foreground 的在途任务标记为 Superseded。
    std::vector<TaskId> supersede_topic(const std::string& topic_key,
                                        TaskId except = kInvalidTaskId);

    // 取消所有满足给定优先级的任务。
    std::vector<TaskId> cancel_by_priority(TaskPriority p);

    // ---- 查询 ----
    std::optional<TaskSnapshot> get(TaskId id) const;
    std::vector<TaskSnapshot> active() const;
    std::vector<TaskOutcome> take_results(TaskId id);   // 取走并清空结果
    std::optional<TaskOutcome> latest_result(TaskId id) const;
    size_t active_count() const;

    // 清掉所有已结束任务的记录（会话切换时用）。
    void prune_finished();

private:
    struct TaskEntry {
        TaskSpec spec;
        TaskId id{0};
        TaskState state{TaskState::Queued};
        CancelToken::Ptr token;
        int progress{0};
        double started_ms{0.0};
        double elapsed_ms{0.0};
        std::vector<TaskOutcome> results;
        // 是否已计入 active_ 计数。
        // 显式标志而非从 state 推断 —— Queued/Paused/Running 三个状态下
        // 任务都可能"已计入但不会再跑"，靠状态猜会漏减或重复减。
        bool active_counted{true};
    };

    using Clock = std::chrono::steady_clock;
    static double now_ms();

    void worker_loop_();
    // 阻塞地等一个可执行任务；返回 false 表示退出
    bool next_task_(std::unique_lock<std::mutex>& lk, std::shared_ptr<TaskEntry>& out);
    void run_task_(const std::shared_ptr<TaskEntry>& entry);
    void finish_(const std::shared_ptr<TaskEntry>& entry, TaskOutcome outcome);
    void publish_task_event_(EventType type, const std::shared_ptr<TaskEntry>& entry,
                            const std::string& payload = {});
    // 递减 active_ 计数（幂等：靠 active_counted 保证只减一次）
    void dec_active_locked_(const std::shared_ptr<TaskEntry>& entry);
    static bool is_terminal(TaskState s);

    mutable std::mutex mutex_;
    std::condition_variable cv_;             // 有新任务 / 状态变化
    std::condition_variable pause_cv_;       // 暂停任务在此等待

    std::deque<std::shared_ptr<TaskEntry>> q_foreground_;
    std::deque<std::shared_ptr<TaskEntry>> q_background_;
    std::deque<std::shared_ptr<TaskEntry>> q_speculative_;

    std::unordered_map<TaskId, std::shared_ptr<TaskEntry>> tasks_;
    std::vector<std::thread> workers_;
    ResultSink sink_;

    // 构造时的 worker 数（restart 时复用）
    int worker_count_{3};

    TaskId next_id_{1};
    std::atomic<bool> running_{false};
    std::atomic<size_t> active_{0};
};

}  // namespace voice_agent
