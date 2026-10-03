// src/core/task.cpp
#include "core/task.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>

namespace voice_agent {

using namespace std::chrono_literals;

// ========== TaskContext ==========

void TaskContext::progress(int percent, const std::string& note) {
    Event e{EventType::TaskProgress, 0, note, {},
            note.empty() ? std::to_string(percent) : note};
    e.task_id = id_;
    global_event_bus().publish(std::move(e));
}

bool TaskContext::wait_or_cancel(int timeout_ms) {
    if (cancelled()) return false;
    // 分片睡眠，保证取消/超化能被及时观察（上限 20ms 粒度）
    int left = timeout_ms;
    while (left > 0) {
        int slice = std::min(left, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        left -= slice;
        if (cancelled()) return false;
    }
    return !cancelled();
}

// ========== TaskManager ==========

TaskManager::TaskManager(int workers)
    : worker_count_(std::max(1, workers)) {
    running_.store(false);
}

TaskManager::~TaskManager() {
    stop();
}

void TaskManager::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;   // 已在运行，幂等
    }
    workers_.clear();
    workers_.reserve(worker_count_);
    for (int i = 0; i < worker_count_; ++i) {
        workers_.emplace_back([this] { worker_loop_(); });
    }
    LOG_INFO("TaskManager started with {} workers", worker_count_);
}

void TaskManager::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, e] : tasks_) {
            if (e->state == TaskState::Queued || e->state == TaskState::Paused ||
                e->state == TaskState::Running) {
                if (e->token) e->token->cancel();
                e->state = TaskState::Cancelled;
                dec_active_locked_(e);
            }
        }
    }
    cv_.notify_all();
    pause_cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    LOG_INFO("TaskManager stopped");
}

double TaskManager::now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

TaskId TaskManager::submit(TaskSpec spec) {
    // 懒启动：忘记显式 start() 时也不该静默丢任务
    if (!running_.load()) start();

    auto entry = std::make_shared<TaskEntry>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entry->id = next_id_++;
        entry->spec = std::move(spec);
        entry->token = std::make_shared<CancelToken>();
        entry->state = TaskState::Queued;
        entry->started_ms = now_ms();

        TaskId id = entry->id;
        tasks_[id] = entry;

        switch (entry->spec.priority) {
            case TaskPriority::Foreground:   q_foreground_.push_back(entry); break;
            case TaskPriority::Background:   q_background_.push_back(entry); break;
            case TaskPriority::Speculative: q_speculative_.push_back(entry); break;
        }
        active_.fetch_add(1);
        LOG_DEBUG("Task submitted #{} {} prio={} topic='{}'",
                  id, entry->spec.name,
                  task_priority_to_string(entry->spec.priority),
                  entry->spec.topic_key);
    }
    cv_.notify_one();
    return entry->id;
}

bool TaskManager::next_task_(std::unique_lock<std::mutex>& lk,
                            std::shared_ptr<TaskEntry>& out) {
    while (running_.load()) {
        // 前台优先；只要前台队列非空就不启动新的后台/投机任务（保留位）
        auto pick = [this](std::deque<std::shared_ptr<TaskEntry>>& q)
            -> std::shared_ptr<TaskEntry> {
            while (!q.empty()) {
                auto e = q.front();
                q.pop_front();
                if (e->state == TaskState::Queued) return e;
                // Paused / Cancelled / Superseded-while-queued：直接丢弃
            }
            return nullptr;
        };

        if (!q_foreground_.empty()) {
            if (auto e = pick(q_foreground_)) { out = e; return true; }
            continue;
        }
        if (!q_background_.empty()) {
            if (auto e = pick(q_background_)) { out = e; return true; }
            continue;
        }
        if (!q_speculative_.empty()) {
            if (auto e = pick(q_speculative_)) { out = e; return true; }
            continue;
        }
        cv_.wait_for(lk, 50ms);
    }
    return false;
}

bool TaskManager::is_terminal(TaskState s) {
    return s == TaskState::Completed || s == TaskState::Cancelled ||
           s == TaskState::Failed;
}

void TaskManager::dec_active_locked_(const std::shared_ptr<TaskEntry>& entry) {
    if (entry->active_counted) {
        entry->active_counted = false;
        active_.fetch_sub(1);
    }
}

void TaskManager::worker_loop_() {
    while (true) {
        std::shared_ptr<TaskEntry> entry;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            if (!next_task_(lk, entry)) return;
            if (!entry) continue;
            if (entry->token && entry->token->cancelled()) {
                entry->state = TaskState::Cancelled;
                dec_active_locked_(entry);
                continue;
            }
            entry->state = TaskState::Running;
            entry->started_ms = now_ms();
        }
        publish_task_event_(EventType::TaskStarted, entry);
        run_task_(entry);
    }
}

void TaskManager::run_task_(const std::shared_ptr<TaskEntry>& entry) {
    TaskId id = entry->id;
    // 让 TaskContext 能读到 TaskManager 里的真实 supersede 状态
    std::weak_ptr<TaskEntry> weak = entry;
    auto is_sup = [this, weak]() -> bool {
        auto e = weak.lock();
        if (!e) return true;   // entry 已销毁 => 视为被取代
        std::lock_guard<std::mutex> lock(mutex_);
        return e->state == TaskState::Superseded;
    };
    TaskContext ctx(id, entry->spec.name, entry->token, is_sup);
    TaskOutcome outcome;

    // 暂停支持：PauseOnInterrupt 的任务在开跑前/跑到一半都可停在 pause_cv
    auto wait_if_paused = [this, &entry]() -> bool {
        std::unique_lock<std::mutex> lk(mutex_);
        while (entry->state == TaskState::Paused && running_.load()) {
            if (entry->token && entry->token->cancelled()) return false;
            pause_cv_.wait_for(lk, 20ms);
        }
        return entry->state != TaskState::Cancelled;
    };

    if (!wait_if_paused()) {
        outcome.ok = false;
        outcome.error = "cancelled";
        finish_(entry, std::move(outcome));
        return;
    }

    const auto deadline = entry->spec.timeout_ms > 0
        ? Clock::now() + std::chrono::milliseconds(entry->spec.timeout_ms)
        : Clock::time_point::max();

    try {
        outcome = entry->spec.work(ctx);
    } catch (const std::exception& e) {
        outcome.ok = false;
        outcome.error = std::string("task threw: ") + e.what();
    } catch (...) {
        outcome.ok = false;
        outcome.error = "task threw unknown exception";
    }

    if (entry->token && entry->token->cancelled() && !outcome.error.empty()) {
        // 取消导致的失败统一归类
    }
    if (Clock::now() > deadline) {
        outcome.ok = false;
        outcome.error = "task timeout";
    }

    // 播报许可：被 supersede 的任务结果只入 cache
    if (ctx.superseded()) outcome.should_speak = false;
    finish_(entry, std::move(outcome));
}

void TaskManager::publish_task_event_(EventType type,
                                      const std::shared_ptr<TaskEntry>& entry,
                                      const std::string& payload) {
    Event e{type, 0, entry->spec.name, {}, payload};
    e.task_id = entry->id;
    e.turn_id = entry->spec.turn_id;
    global_event_bus().publish(std::move(e));
}

void TaskManager::finish_(const std::shared_ptr<TaskEntry>& entry, TaskOutcome outcome) {
    TaskId id = entry->id;
    EventType ev = EventType::TaskCompleted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outcome.elapsed_ms = now_ms() - entry->started_ms;
        entry->elapsed_ms = outcome.elapsed_ms;
        entry->results.push_back(outcome);

        if (entry->state == TaskState::Cancelled) {
            ev = EventType::TaskCancelled;
        } else if (entry->state == TaskState::Superseded) {
            ev = EventType::TaskSuperseded;
        } else if (entry->token && entry->token->cancelled()) {
            // 取消令牌已触发但状态未被显式置位（如 work 内部自行观测到取消）
            entry->state = TaskState::Cancelled;
            ev = EventType::TaskCancelled;
        } else if (!outcome.ok) {
            entry->state = TaskState::Failed;
            ev = EventType::TaskFailed;
        } else {
            entry->state = TaskState::Completed;
        }
        dec_active_locked_(entry);
    }

    publish_task_event_(ev, entry, outcome.error);
    LOG_DEBUG("Task #{} finished state={} ok={} {:.0f}ms speak={}",
              id, task_state_to_string(entry->state), outcome.ok,
              outcome.elapsed_ms, outcome.should_speak);

    ResultSink sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink = sink_;
    }
    // 结果一律回调（由上层决定入 cache 还是播报），不在此处阻塞
    if (sink) sink(id, outcome);
}

// ---- 单任务控制 ----

bool TaskManager::cancel(TaskId id) {
    std::shared_ptr<TaskEntry> e;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return false;
        e = it->second;
        if (is_terminal(e->state) || e->state == TaskState::Superseded) {
            return false;
        }
        // 尚未开始的任务：立刻出队、终结计数。
        // 已在跑的：只置状态 + 取消令牌，计数留给 finish_ 收敛。
        if (e->state == TaskState::Queued || e->state == TaskState::Paused) {
            e->state = TaskState::Cancelled;
            dec_active_locked_(e);
        } else {
            e->state = TaskState::Cancelled;
        }
    }
    if (e->token) e->token->cancel();
    cv_.notify_all();
    pause_cv_.notify_all();
    publish_task_event_(EventType::TaskCancelled, e, "cancelled by user");
    return true;
}

bool TaskManager::pause(TaskId id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return false;
        if (it->second->state != TaskState::Running &&
            it->second->state != TaskState::Queued) {
            return false;
        }
        it->second->state = TaskState::Paused;
    }
    pause_cv_.notify_all();
    return true;
}

bool TaskManager::resume(TaskId id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return false;
        if (it->second->state != TaskState::Paused) return false;
        it->second->state = TaskState::Running;
    }
    pause_cv_.notify_all();
    cv_.notify_all();
    return true;
}

bool TaskManager::supersede(TaskId id) {
    std::shared_ptr<TaskEntry> e;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return false;
        e = it->second;
        if (is_terminal(e->state) || e->state == TaskState::Superseded) {
            return false;
        }
        // 尚未开始的任务：立刻出队、终结计数。
        // 已在跑的：只打标记（不 cancel —— 让它跑完，结果入 cache）
        if (e->state == TaskState::Queued || e->state == TaskState::Paused) {
            e->state = TaskState::Superseded;
            dec_active_locked_(e);
        } else {
            e->state = TaskState::Superseded;
        }
    }
    pause_cv_.notify_all();
    cv_.notify_all();
    publish_task_event_(EventType::TaskSuperseded, e, "superseded");
    return true;
}

std::vector<TaskId> TaskManager::apply_interrupt() {
    std::vector<std::shared_ptr<TaskEntry>> to_cancel, to_pause;
    std::vector<TaskId> affected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, e] : tasks_) {
            if (is_terminal(e->state) || e->state == TaskState::Superseded) continue;
            switch (e->spec.policy) {
                case TaskPolicy::CancelOnInterrupt:
                    to_cancel.push_back(e);
                    break;
                case TaskPolicy::PauseOnInterrupt:
                    to_pause.push_back(e);
                    break;
                case TaskPolicy::ContinueOnInterrupt:
                case TaskPolicy::SupersedeOnNewTopic:
                    // 继续跑完，但结果不播报。
                    // 尚未开始的直接出队终结；在跑的留给 finish_ 收敛。
                    if (e->state == TaskState::Queued || e->state == TaskState::Paused) {
                        e->state = TaskState::Superseded;
                        dec_active_locked_(e);
                    } else {
                        e->state = TaskState::Superseded;
                    }
                    break;
            }
            affected.push_back(id);
        }
    }
    for (auto& e : to_pause) pause(e->id);
    for (auto& e : to_cancel) cancel(e->id);
    return affected;
}

std::vector<TaskId> TaskManager::supersede_topic(const std::string& topic_key,
                                                 TaskId except) {
    std::vector<TaskId> affected;
    if (topic_key.empty()) return affected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, e] : tasks_) {
            if (id == except) continue;
            if (e->spec.topic_key != topic_key) continue;
            if (is_terminal(e->state) || e->state == TaskState::Superseded) continue;
            // 前台任务不参与 supersede：它就是当前话题本身
            if (e->spec.priority == TaskPriority::Foreground) continue;
            if (e->state == TaskState::Queued || e->state == TaskState::Paused) {
                e->state = TaskState::Superseded;
                dec_active_locked_(e);
            } else {
                e->state = TaskState::Superseded;
            }
            affected.push_back(id);
        }
    }
    for (auto id : affected) {
        LOG_INFO("Task #{} superseded (topic='{}')", id, topic_key);
    }
    pause_cv_.notify_all();
    cv_.notify_all();
    return affected;
}

std::vector<TaskId> TaskManager::cancel_by_priority(TaskPriority p) {
    std::vector<TaskId> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, e] : tasks_) {
            if (e->spec.priority != p) continue;
            if (is_terminal(e->state) || e->state == TaskState::Superseded) continue;
            ids.push_back(id);
        }
    }
    for (auto id : ids) cancel(id);
    return ids;
}

// ---- 查询 ----

std::optional<TaskSnapshot> TaskManager::get(TaskId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return std::nullopt;
    const auto& e = it->second;
    TaskSnapshot s;
    s.id = e->id;
    s.name = e->spec.name;
    s.topic_key = e->spec.topic_key;
    s.priority = e->spec.priority;
    s.policy = e->spec.policy;
    s.state = e->state;
    s.turn_id = e->spec.turn_id;
    s.progress = e->progress;
    s.elapsed_ms = e->elapsed_ms;
    if (!e->results.empty()) s.should_speak = e->results.back().should_speak;
    return s;
}

std::vector<TaskSnapshot> TaskManager::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TaskSnapshot> out;
    for (auto& [id, e] : tasks_) {
        if (is_terminal(e->state) || e->state == TaskState::Superseded) continue;
        TaskSnapshot s;
        s.id = e->id;
        s.name = e->spec.name;
        s.topic_key = e->spec.topic_key;
        s.priority = e->spec.priority;
        s.state = e->state;
        s.turn_id = e->spec.turn_id;
        s.elapsed_ms = e->elapsed_ms;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<TaskOutcome> TaskManager::take_results(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return {};
    std::vector<TaskOutcome> out;
    out.swap(it->second->results);
    return out;
}

std::optional<TaskOutcome> TaskManager::latest_result(TaskId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end() || it->second->results.empty()) return std::nullopt;
    return it->second->results.back();
}

size_t TaskManager::active_count() const {
    return active_.load();
}

void TaskManager::prune_finished() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (is_terminal(it->second->state)) {
            it = tasks_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace voice_agent
