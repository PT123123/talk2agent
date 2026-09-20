// src/core/cancel_token.hpp
#pragma once
#include <atomic>
#include <functional>
#include <vector>
#include <memory>
#include <mutex>

// ========== 级联取消令牌 ==========
// 用于 LLM 生成、TTS 合成、工具执行等的统一取消传播

class CancelToken : public std::enable_shared_from_this<CancelToken> {
public:
    using Ptr = std::shared_ptr<CancelToken>;

    CancelToken() = default;
    ~CancelToken() {
        // 确保析构时触发所有回调
        if (!cancelled() && !on_cancel_callbacks_.empty()) {
            cancel();
        }
    }

    // 检查是否已取消
    bool cancelled() const {
        return cancelled_.load(std::memory_order_acquire);
    }

    // 取消此令牌（传播给所有子令牌）
    void cancel() {
        if (cancelled_.exchange(true, std::memory_order_acq_rel)) {
            return;  // 已经取消过了
        }

        // 执行所有回调
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks = std::move(on_cancel_callbacks_);
        }

        for (auto& cb : callbacks) {
            cb();
        }

        // 传播给子令牌
        std::vector<Ptr> children;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            children = std::move(children_);
        }
        for (auto& child : children) {
            if (child) child->cancel();
        }
    }

    // 添加子令牌（子令牌取消时父令牌也会取消）
    void add_child(Ptr child) {
        if (!child) return;
        std::lock_guard<std::mutex> lock(mutex_);
        children_.push_back(child);
        // 如果父令牌已取消，立即取消子令牌
        if (cancelled_.load(std::memory_order_relaxed)) {
            child->cancel();
        }
    }

    // 注册取消时的回调
    void on_cancel(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_.load(std::memory_order_relaxed)) {
            callback();
        } else {
            on_cancel_callbacks_.push_back(std::move(callback));
        }
    }

    // 创建新的子令牌
    Ptr make_child() {
        auto child = std::make_shared<CancelToken>();
        add_child(child);
        return child;
    }

private:
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::vector<std::function<void()>> on_cancel_callbacks_;
    std::vector<Ptr> children_;
};

// ========== 取消令牌包装器 ==========
// RAII 风格的取消令牌管理

class CancelGuard {
public:
    explicit CancelGuard(CancelToken::Ptr token) : token_(std::move(token)) {}
    ~CancelGuard() {
        if (token_) token_->cancel();
    }

    CancelGuard(const CancelGuard&) = delete;
    CancelGuard& operator=(const CancelGuard&) = delete;

    CancelGuard(CancelGuard&&) = default;
    CancelGuard& operator=(CancelGuard&&) = default;

    bool cancelled() const { return token_ && token_->cancelled(); }

    CancelToken::Ptr get_token() { return token_; }

private:
    CancelToken::Ptr token_;
};
