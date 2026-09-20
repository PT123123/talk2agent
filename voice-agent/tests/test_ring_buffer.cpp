// tests/test_ring_buffer.cpp
#include "../src/core/ring_buffer.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <cassert>
#include <random>

using namespace std;

// 测试基本功能
void test_basic() {
    cout << "Test basic push/pop..." << endl;
    SpscRingBuffer<int, 1024> rb;

    // 空 buffer
    assert(rb.available() == 0);

    // 写入
    int data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    size_t written = rb.push(data, 10);
    assert(written == 10);
    assert(rb.available() == 10);

    // 读取
    int out[10];
    size_t read = rb.pop(out, 5);
    assert(read == 5);
    assert(rb.available() == 5);
    for (int i = 0; i < 5; i++) assert(out[i] == i + 1);

    // 继续读
    read = rb.pop(out, 10);
    assert(read == 5);
    for (int i = 0; i < 5; i++) assert(out[i] == i + 6);

    assert(rb.available() == 0);
    cout << "  PASS" << endl;
}

// 测试覆盖
void test_overwrite() {
    cout << "Test overwrite..." << endl;
    SpscRingBuffer<int, 16> rb;

    int data[32];
    for (int i = 0; i < 32; i++) data[i] = i;

    rb.push(data, 32);
    assert(rb.available() == 16);  // 满时会覆盖最旧的

    int out[16];
    rb.pop(out, 16);
    // 应该读到的是最后 16 个: 16-31
    for (int i = 0; i < 16; i++) {
        assert(out[i] == i + 16);
    }
    cout << "  PASS" << endl;
}

// 多线程测试（SPSC）
// 模拟真实音频场景：48kHz，每 10ms 480 样本，双方速率匹配
void test_multithread() {
    cout << "Test multi-thread SPSC (48kHz, 10ms batches, 10s)..." << endl;
    // 32768 样本 ≈ 683ms 缓冲余量，足以吸收 producer/consumer 之间的时序抖动
    SpscRingBuffer<int, 32768> rb;

    atomic<bool> stop{false};
    atomic<size_t> produced{0};
    atomic<size_t> consumed{0};

    // 生产者：每 10ms 推送 480 样本（48kHz 真实速率）
    thread producer([&]() {
        constexpr size_t BATCH = 480;
        int batch[BATCH];
        size_t seq = 0;
        auto next_time = chrono::steady_clock::now();
        while (!stop.load()) {
            for (size_t i = 0; i < BATCH; i++) {
                batch[i] = static_cast<int>(seq++);
            }
            produced.fetch_add(rb.push(batch, BATCH));
            next_time += chrono::milliseconds(10);
            this_thread::sleep_until(next_time);
        }
    });

    // 消费者：无延迟尽力读，保持与 producer 速率匹配
    thread consumer([&]() {
        constexpr size_t BATCH = 480;
        int batch[BATCH];
        size_t expected = 0;
        while (!stop.load() || rb.available() > 0) {
            size_t got = rb.pop(batch, BATCH);
            for (size_t i = 0; i < got; i++) {
                if (batch[i] != static_cast<int>(expected++)) {
                    cerr << "Data mismatch at " << i << ", expected " << expected - 1
                         << " got " << batch[i] << endl;
                    assert(false);
                }
            }
            consumed.fetch_add(got);
            // 无 sleep：consumer 追 producer，producer 每 10ms 推 480，consumer 读 480，平均速率相同
        }
    });

    this_thread::sleep_for(chrono::seconds(10));
    stop.store(true);
    producer.join();
    consumer.join();

    size_t p = produced.load(), c = consumed.load();
    cout << "  Produced: " << p << ", Consumed: " << c << endl;
    assert(p == c);  // 速率匹配时 buffer 不会溢出，所有数据都被消费
    cout << "  PASS" << endl;
}

// 性能测试
void test_performance() {
    cout << "Test performance (10万帧)..." << endl;
    SpscRingBuffer<int16_t, 32768> rb;

    vector<int16_t> data(480);  // 10ms @ 48kHz
    for (int i = 0; i < 480; i++) data[i] = static_cast<int16_t>(i);

    // 单线程压测
    for (int iter = 0; iter < 100000; iter++) {
        rb.push(data.data(), 480);
        rb.pop(data.data(), 480);
    }

    cout << "  100k iterations completed" << endl;
    cout << "  PASS" << endl;
}

int main() {
    cout << "=== Ring Buffer Tests ===" << endl;

    test_basic();
    test_overwrite();
    test_multithread();
    test_performance();

    cout << "\nAll tests passed!" << endl;
    return 0;
}
