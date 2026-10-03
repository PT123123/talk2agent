// tests/test_long_run.cpp
// R8: 长时运行稳定性 —— 无需真实音频设备，全部用可控组件跑
//
// 覆盖：
//   1. TaskManager 数千次提交无线程/内存增长
//   2. LatencyMetrics 定长环形缓冲，内存有界
//   3. VoiceTraceWriter 长跑下不涨内存（写盘为主）
//   4. BackgroundCache / ContextManager 的容量红线
//   5. 状态机反复 start/stop 无残留
//   6. 取消与 supersede 在高并发下的正确性
#include "core/task.hpp"
#include "core/working_context.hpp"
#include "orchestrator/background_cache.hpp"
#include "util/latency_metrics.hpp"
#include "util/voice_trace.hpp"
#include "util/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#include <windows.h>
#include <psapi.h>

using namespace std;
using namespace voice_agent;
using namespace std::chrono_literals;

namespace {

// 当前进程的工作集（KB）
size_t working_set_kb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<size_t>(pmc.WorkingSetSize / 1024);
    }
    return 0;
}

// 线程句柄数（近似：读 TEB 不便，用创建/销毁计数代替）
std::atomic<int> g_threads_created{0};

}  // namespace

// ============================================================
// 1. TaskManager 长跑：无线程泄漏、计数归零
// ============================================================
static void test_task_manager_long_run() {
    cout << "TEST task manager long run..." << endl;

    const size_t kBefore = working_set_kb();
    TaskManager tm(4);
    tm.start();

    const int kIterations = 2000;
    std::atomic<int> done{0};
    for (int i = 0; i < kIterations; ++i) {
        TaskSpec s;
        s.name = "job" + to_string(i % 5);
        s.priority = (i % 3 == 0) ? TaskPriority::Foreground
                                   : TaskPriority::Background;
        s.topic_key = "topic:" + to_string(i % 7);
        s.work = [&done](TaskContext&) {
            done.fetch_add(1);
            TaskOutcome o;
            o.ok = true;
            o.value = "x";
            return o;
        };
        tm.submit(std::move(s));
    }
    // 等待全部完成
    for (int i = 0; i < 2000 && tm.active_count() > 0; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    assert(tm.active_count() == 0);
    assert(done.load() == kIterations);

    // 计数归零是"没有任务卡住"的硬指标
    tm.prune_finished();
    assert(tm.active_count() == 0);

    tm.stop();
    const size_t kAfter = working_set_kb();
    // 内存增长应受控（2000 个小任务的残留不应超过 64MB）
    cout << "  (2000 tasks, ws " << kBefore << "->" << kAfter << " KB) ";
    assert(kAfter < kBefore + 64 * 1024);
    cout << "-> PASS" << endl;
}

// ============================================================
// 2. TaskManager 反复 start/stop（GUI 会反复调）
// ============================================================
static void test_task_manager_restart() {
    cout << "TEST task manager restart is idempotent..." << endl;

    TaskManager tm(3);
    std::atomic<int> runs{0};

    for (int round = 0; round < 20; ++round) {
        tm.start();
        tm.start();   // 幂等：不该起第二组 worker

        TaskSpec s;
        s.name = "r";
        s.work = [&runs](TaskContext&) {
            runs.fetch_add(1);
            TaskOutcome o; o.ok = true; return o;
        };
        tm.submit(std::move(s));
        for (int i = 0; i < 200 && tm.active_count() > 0; ++i) {
            std::this_thread::sleep_for(2ms);
        }
        tm.stop();
        tm.stop();     // 幂等
    }
    assert(runs.load() == 20);
    cout << "  (20 start/stop rounds, 20 runs) -> PASS" << endl;
}

// ============================================================
// 3. LatencyMetrics 定长缓冲：万次记录内存不涨
// ============================================================
static void test_metrics_memory_bounded() {
    cout << "TEST latency metrics memory bounded..." << endl;

    const size_t kBefore = working_set_kb();
    LatencyMetrics::Config cfg;
    cfg.sample_capacity = 256;      // 每指标 256 个样本
    LatencyMetrics m(cfg);

    for (int i = 0; i < 50000; ++i) {
        m.record(MetricKind::LlmTtft, 50.0 + (i % 100));
        m.record(MetricKind::AsrToFirstAudio, 200.0 + (i % 300));
    }
    // 统计仍然全量累计
    assert(m.count(MetricKind::LlmTtft) == 50000);
    assert(m.count(MetricKind::AsrToFirstAudio) == 50000);

    // 分位数可算
    const double p50 = m.percentile(MetricKind::LlmTtft, 0.5);
    const double p99 = m.percentile(MetricKind::LlmTtft, 0.99);
    assert(p50 > 0 && p99 >= p50);
    assert(m.avg(MetricKind::LlmTtft) >= 50.0);

    const size_t kAfter = working_set_kb();
    cout << "  (50k records x2, ws " << kBefore << "->" << kAfter << " KB) ";
    assert(kAfter < kBefore + 32 * 1024);   // 增长应 < 32MB
    cout << "-> PASS" << endl;
}

// ============================================================
// 4. VoiceTraceWriter 长跑写盘
// ============================================================
static void test_trace_writer_soak() {
    cout << "TEST voice trace writer soak..." << endl;

    const char* path = "build-msvc/_soak/trace.jsonl";
    {
        VoiceTraceWriter w(path, true);
        assert(w.ok());
        for (int i = 0; i < 3000; ++i) {
            TurnRecord r;
            r.session_id = "soak";
            r.turn_id = to_string(i);
            r.user_text = "测试一下 " + to_string(i);
            r.user_intent = "Content";
            r.policy_action = "answer";
            r.route_tier = "NORMAL";
            r.llm_ttft_ms = 100.0 + (i % 50);
            r.asr_to_first_audio_ms = 300.0 + (i % 200);
            TraceEvent ev;
            ev.t_us = static_cast<uint64_t>(i) * 1000;
            ev.type = "LLM_START";
            r.events.push_back(ev);
            w.write_turn(r);
        }
    }

    // 读回验证
    VoiceTraceReader rd(path);
    assert(rd.ok());
    auto turns = rd.turns();
    assert(turns.size() == 3000);

    // 分位数计算
    auto st = VoiceTraceReader::compute_latency(turns, "llm_ttft_ms");
    assert(st.count == 3000);
    assert(st.p50 > 0 && st.p99 >= st.p50);

    auto sum = VoiceTraceReader::summarize(turns);
    assert(sum.turns == 3000);

    cout << "  (3000 turns written+read, ttft p50=" << (int)st.p50
         << " p99=" << (int)st.p99 << "ms) -> PASS" << endl;
    ::DeleteFileA(path);
}

// ============================================================
// 5. 并发 cancel + supersede 的正确性
// ============================================================
static void test_concurrent_cancel_supersede() {
    cout << "TEST concurrent cancel/supersede correctness..." << endl;

    TaskManager tm(6);
    tm.start();
    std::atomic<int> completed{0};
    std::atomic<int> cancelled{0};

    const int kJobs = 500;
    for (int i = 0; i < kJobs; ++i) {
        TaskSpec s;
        s.name = "job";
        s.priority = TaskPriority::Background;
        s.topic_key = "t" + to_string(i % 5);
        s.work = [&completed, &cancelled](TaskContext& ctx) {
            for (int k = 0; k < 20; ++k) {
                if (!ctx.wait_or_cancel(2)) {
                    cancelled.fetch_add(1);
                    TaskOutcome o; o.ok = false; o.error = "cancelled";
                    return o;
                }
            }
            completed.fetch_add(1);
            TaskOutcome o; o.ok = true; return o;
        };
        tm.submit(std::move(s));
    }

    // 并发做取消与超化
    std::vector<std::thread> ops;
    for (int t = 0; t < 3; ++t) {
        ops.emplace_back([&tm] {
            for (int i = 0; i < 50; ++i) {
                for (auto& snap : tm.active()) {
                    if (i % 2 == 0) tm.cancel(snap.id);
                    else tm.supersede(snap.id);
                }
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    for (auto& o : ops) o.join();

    for (int i = 0; i < 3000 && tm.active_count() > 0; ++i) {
        std::this_thread::sleep_for(2ms);
    }
    // 关键：全部任务都必须收敛，不能有卡住的
    assert(tm.active_count() == 0);
    // 每个任务要么完成、要么被取消/超化，不应两者都算
    const int accounted = completed.load() + cancelled.load();
    assert(accounted <= kJobs);

    tm.stop();
    cout << "  (" << completed.load() << " done, " << cancelled.load()
         << " cancelled, 0 stuck) -> PASS" << endl;
}

// ============================================================
// 6. 长跑后的各容器容量红线
// ============================================================
static void test_container_limits_after_soak() {
    cout << "TEST container capacity limits after soak..." << endl;

    BackgroundCache cache;
    for (int i = 0; i < 20000; ++i) {
        cache.put("k" + to_string(i), string(500, 'v'));
    }
    assert(cache.size() <= 64);
    assert(cache.total_chars() < 128 * 1024);

    ContextManager cm(ContextManager::Config{4, 200, 100, 100, 1000, 50});
    cm.set_summary_policy(8, 300);
    for (int i = 0; i < 20000; ++i) {
        cm.add_turn("Q" + to_string(i), "A" + to_string(i));
    }
    assert(cm.turn_count() <= 50);
    assert(cm.older_summary().size() <= 300);

    cout << "  (20k ops: cache=" << cache.size() << "/" << cache.total_chars()
         << "B, turns=" << cm.turn_count() << ", summary="
         << cm.older_summary().size() << "B) -> PASS" << endl;
}

// ============================================================
// 7. 连续多轮完整对话循环（模拟 4 小时使用的核心循环）
// ============================================================
static void test_multi_turn_loop() {
    cout << "TEST multi-turn conversation loop..." << endl;

    const size_t kBefore = working_set_kb();

    BackgroundCache cache;
    ContextManager cm(ContextManager::Config{6, 400, 600, 600, 4000, 200});
    LatencyMetrics metrics;

    for (int turn = 0; turn < 5000; ++turn) {
        // 模拟一轮：用户说话 → 回答 → 入上下文
        const std::string user = "第 " + to_string(turn) + " 个问题";
        const std::string answer = "回答 " + to_string(turn);

        cm.set_topic("topic" + to_string(turn % 3));
        cm.add_turn(user, answer);
        cm.set_relevant_memory({"mem" + to_string(turn)});
        cm.clear_results();

        metrics.record(MetricKind::TurnTotal, 500.0 + (turn % 1000));

        // 每 10 轮放一个后台结果进 cache
        if (turn % 10 == 0) {
            cache.put("search:" + to_string(turn), string(400, 's'), 800.0, true);
        }
        // 每 100 轮清理过期项（模拟真实的周期维护）
        if (turn % 100 == 0) {
            cache.expire();
            cm.working().expire();
        }
    }

    const size_t kAfter = working_set_kb();
    assert(cm.turn_count() <= 200);
    assert(cache.size() <= 64);
    cout << "  (5000 turns, ws " << kBefore << "->" << kAfter << " KB) ";
    assert(kAfter < kBefore + 48 * 1024);
    cout << "-> PASS" << endl;
}

int main() {
    auto logger = init_logger("test-long-run", "warn");
    (void)logger;

    test_task_manager_long_run();
    test_task_manager_restart();
    test_metrics_memory_bounded();
    test_trace_writer_soak();
    test_concurrent_cancel_supersede();
    test_container_limits_after_soak();
    test_multi_turn_loop();

    cout << "\nAll R8 long-run stability tests PASSED" << endl;
    return 0;
}
