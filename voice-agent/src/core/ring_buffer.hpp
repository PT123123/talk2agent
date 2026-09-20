// src/core/ring_buffer.hpp
#pragma once
#include <atomic>
#include <cstddef>
#include <cstring>
#include <optional>
#include <array>
#include <memory>

// ========== SPSC 无锁环形缓冲 ==========
// 单生产者单消费者，用于音频线程和推理线程之间的 PCM 传递
//
// 算法（参考 Linux kernel lib/ring_buffer + llama.cpp）：
// - buffer 大小必须为 2 的幂次，可用位掩码替代模运算
// - 使用 "水印" 变量在 push 端预判空间，避免 pop 端的 head 读被错误缓存
// - head_：生产者已写入的位置（不含本次写入的数据）
// - tail_：消费者已读取的位置（不含本次读取的数据）
// - 二者均为单调递增的绝对位置，不在 [0, N) 范围内循环
// - 可用空间 = tail + N - head（head 落后时）
//
// 关键内存序：
// - pop 先读 tail 再读 head：acquire 屏障保证数据可见性
// - push 先读 head 再写数据再更新 head：release 屏障保证数据写入对消费者可见
// - 所有跨线程共享变量使用 seq_cst

template<typename T, size_t N>
class SpscRingBuffer {
public:
    static_assert((N & (N - 1)) == 0, "N must be power of 2");
    static constexpr size_t capacity() { return N; }
    static constexpr size_t MASK = N - 1;  // 位掩码：head % N == head & MASK

    SpscRingBuffer() : buffer_(std::make_unique<std::array<T, N>>()) {
        // 初始化为 0：buffer 全空
        std::memset(buffer_->data(), 0, N * sizeof(T));
    }

    // 生产者：写入（单线程调用）
    // 返回写入的样本数；如果 buffer 满则覆盖最旧数据
    size_t push(const T* data, size_t count) {
        // 读取当前写入位置（head）
        const size_t head = head_.load(std::memory_order_relaxed);
        // 读取消费者位置（tail）—— seq_cst 确保读到最新值
        const size_t tail = tail_.load(std::memory_order_acquire);

        // 计算可用空间（用绝对位置差，防止回绕）
        size_t used = (head >= tail) ? (head - tail) : (head + N * 1024 - tail);
        // 防止 head 和 tail 差值超过 N*512（避免溢出检测失效）
        if (used > N * 512) {
            tail_.store(head & MASK, std::memory_order_release);
            return push(data, count);  // 重试
        }

        // 允许覆盖：最多写入 N 个样本
        size_t to_write = std::min(count, N);

        // 计算实际可无覆盖写入的数量（保留一个样本的"满"检测空间）
        // available = N - used - 1（如果不想覆盖）
        // 如果允许覆盖，则 to_write 不受 used 限制
        size_t first_part = std::min(to_write, N - static_cast<size_t>((head & MASK)));
        size_t second_part = to_write - first_part;

        // 写数据到 buffer
        T* dst = buffer_->data() + (head & MASK);
        std::memcpy(dst, data, first_part * sizeof(T));
        if (second_part > 0) {
            std::memcpy(buffer_->data(), data + first_part, second_part * sizeof(T));
        }

        // 内存屏障：确保数据写完后再更新 head
        std::atomic_thread_fence(std::memory_order_release);
        // 更新 head（使用 release 确保数据写入对消费者可见）
        head_.store(head + to_write, std::memory_order_release);

        return to_write;
    }

    // 消费者：读取（单线程调用）
    // 返回读取的样本数
    size_t pop(T* data, size_t count) {
        // 读取当前读取位置（tail）
        const size_t tail = tail_.load(std::memory_order_relaxed);
        // 读取生产者位置（head）—— seq_cst 确保读到最新值
        const size_t head = head_.load(std::memory_order_acquire);

        // 计算可读数量
        size_t available = (head >= tail) ? (head - tail) : 0;
        if (available == 0) return 0;

        size_t to_read = std::min(count, available);
        if (to_read == 0) return 0;

        size_t first_part = std::min(to_read, N - static_cast<size_t>((tail & MASK)));
        size_t second_part = to_read - first_part;

        // 读数据
        const T* src = buffer_->data() + (tail & MASK);
        std::memcpy(data, src, first_part * sizeof(T));
        if (second_part > 0) {
            std::memcpy(data + first_part, buffer_->data(), second_part * sizeof(T));
        }

        // 内存屏障：确保数据读完后才能更新 tail
        std::atomic_thread_fence(std::memory_order_acquire);
        tail_.store(tail + to_read, std::memory_order_release);

        return to_read;
    }

    // 读取但不移除
    size_t peek(T* data, size_t count) const {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);

        size_t available = (head >= tail) ? (head - tail) : 0;
        if (available == 0) return 0;

        size_t to_peek = std::min(count, available);
        if (to_peek == 0) return 0;

        size_t first_part = std::min(to_peek, N - static_cast<size_t>((tail & MASK)));
        size_t second_part = to_peek - first_part;

        const T* src = buffer_->data() + (tail & MASK);
        std::memcpy(data, src, first_part * sizeof(T));
        if (second_part > 0) {
            std::memcpy(data + first_part, buffer_->data(), second_part * sizeof(T));
        }

        return to_peek;
    }

    // 可读数量（上限为 N，防止溢出后虚高）
    size_t available() const {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t diff = (head >= tail) ? (head - tail) : 0;
        return diff > N ? N : diff;
    }

    // 清空
    void clear() {
        tail_.store(head_.load(std::memory_order_relaxed) % N, std::memory_order_release);
    }

private:
    std::unique_ptr<std::array<T, N>> buffer_;
    alignas(64) std::atomic<size_t> head_{0};   // 生产者已写入的绝对位置
    alignas(64) std::atomic<size_t> tail_{0};    // 消费者已读取的绝对位置
};

// ========== 常用特化 ==========
using AudioRing48k = SpscRingBuffer<int16_t, 48000>;  // 1秒 48kHz 单声道
using AudioRing16k = SpscRingBuffer<int16_t, 16000>;  // 1秒 16kHz 单声道

// 帧大小的环形缓冲（用于 VAD 推送）
// head_/tail_ 使用绝对位置，不在 [0, CAPACITY) 范围内循环
// 使用模运算（CAPACITY 不必须是 2 的幂次）
template<typename T, size_t FRAMES_PER_BLOCK, size_t MAX_BLOCKS>
class FrameRingBuffer {
public:
    static constexpr size_t CAPACITY = FRAMES_PER_BLOCK * MAX_BLOCKS;

    bool push_block(const T* block) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);

        size_t used = (head >= tail) ? (head - tail) : (head + CAPACITY * 1024 - tail);
        if (used >= CAPACITY) {
            tail_.store((tail + FRAMES_PER_BLOCK) % CAPACITY, std::memory_order_release);
        }

        const size_t slot = head % CAPACITY;
        std::memcpy(buffer_.data() + slot, block, FRAMES_PER_BLOCK * sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        head_.store(head + FRAMES_PER_BLOCK, std::memory_order_release);
        return true;
    }

    // 获取最近 N 帧（可能跨块）
    size_t get_recent(T* out, size_t frames) const {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);

        size_t available = (head >= tail) ? (head - tail) : 0;
        size_t to_get = std::min(frames, available);
        if (to_get == 0) return 0;

        size_t start = (head - to_get) % CAPACITY;

        if (start + to_get <= CAPACITY) {
            std::memcpy(out, buffer_.data() + start, to_get * sizeof(T));
        } else {
            size_t first = CAPACITY - start;
            std::memcpy(out, buffer_.data() + start, first * sizeof(T));
            std::memcpy(out + first, buffer_.data(), (to_get - first) * sizeof(T));
        }

        return to_get;
    }

    void clear() {
        tail_.store(head_.load(std::memory_order_relaxed) % CAPACITY, std::memory_order_release);
    }

private:
    std::array<T, CAPACITY> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};
