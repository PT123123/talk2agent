// src/core/event_bus.hpp
#pragma once
#include "types.hpp"
#include <atomic>
#include <functional>
#include <vector>
#include <mutex>
#include <queue>
#include <condition_variable>

// ========== 无锁事件总线 ==========
// 线程安全的事件发布-订阅系统

using EventCallback = std::function<void(const Event&)>;

class EventBus {
public:
    using Token = size_t;

    // 订阅事件
    Token subscribe(EventType type, EventCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        Token id = next_token_++;
        subscriptions_[static_cast<size_t>(type)].push_back({id, std::move(callback)});
        return id;
    }

    // 订阅多个事件类型
    Token subscribe(const std::vector<EventType>& types, EventCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        Token id = next_token_++;
        for (auto type : types) {
            subscriptions_[static_cast<size_t>(type)].push_back({id, callback});
        }
        return id;
    }

    // 取消订阅
    void unsubscribe(Token id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& sub_list : subscriptions_) {
            sub_list.erase(
                std::remove_if(sub_list.begin(), sub_list.end(),
                    [id](const auto& s) { return s.id == id; }),
                sub_list.end()
            );
        }
    }

    // 发布事件（异步，内部队列）
    void publish(Event event) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            pending_events_.push(std::move(event));
        }
        cv_.notify_one();
    }

    // 同步发布（直接在调用线程执行回调）
    void publish_sync(const Event& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& subs = subscriptions_[static_cast<size_t>(event.type)];
        for (auto& sub : subs) {
            sub.callback(event);
        }
    }

    // 事件处理循环（供消费者线程调用）
    void process_events() {
        while (true) {
            Event event;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !pending_events_.empty();
                });
                if (pending_events_.empty()) return;

                event = std::move(pending_events_.front());
                pending_events_.pop();
            }

            // 在锁外执行回调
            std::vector<EventCallback> callbacks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto& subs = subscriptions_[static_cast<size_t>(event.type)];
                for (auto& sub : subs) {
                    callbacks.push_back(sub.callback);
                }
            }

            for (auto& cb : callbacks) {
                cb(event);
            }
        }
    }

    // 停止事件循环
    void stop() {
        publish(Event{EventType::Error});  // 发送哨兵事件
    }

    // 清空待处理事件
    void clear() {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::queue<Event> empty;
        pending_events_.swap(empty);
    }

private:
    struct Subscription {
        Token id;
        EventCallback callback;
    };

    std::mutex mutex_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::vector<Subscription> subscriptions_[static_cast<size_t>(EventType::Error) + 1];
    std::queue<Event> pending_events_;
    Token next_token_{1};
};

// ========== 全局事件总线单例 ==========
inline EventBus& global_event_bus() {
    static EventBus bus;
    return bus;
}
