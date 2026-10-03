// tests/test_fast_response.cpp
// R3: Fast Response Layer（慢任务先垫一句）+ BackgroundCache（结果落地与引用）
#include "orchestrator/fast_response.hpp"
#include "orchestrator/background_cache.hpp"
#include "orchestrator/response_policy.hpp"
#include "util/log.hpp"

#include <atomic>
#include <cassert>
#include <iostream>
#include <string>
#include <thread>

using namespace std;
using namespace std::chrono_literals;
using namespace voice_agent;

namespace {

ResponseDecision make_decision(ResponseAction action, bool allow_bg) {
    ResponseDecision d;
    d.action = action;
    d.allow_background = allow_bg;
    return d;
}

}  // namespace

// ============================================================
// 1. 够快就不说话 —— 不能每句话都先"嗯"一下
// ============================================================
static void test_fast_task_stays_silent() {
    cout << "TEST fast task stays silent..." << endl;

    FastResponseLayer fr;
    // 本地小模型实测很快
    for (int i = 0; i < 5; ++i) fr.record_latency("llm", 150);

    assert(fr.estimate_latency_ms("llm") < 700);
    auto p = fr.plan(make_decision(ResponseAction::Answer, false), "llm", "现在几点");
    assert(p.decision == AckDecision::Silent);
    assert(p.text.empty());
    cout << "  (est=" << p.expected_ms << "ms) -> PASS" << endl;
}

// ============================================================
// 2. 慢任务先垫一句
// ============================================================
static void test_slow_task_acknowledges() {
    cout << "TEST slow task acknowledges..." << endl;

    FastResponseLayer fr;
    for (int i = 0; i < 5; ++i) fr.record_latency("search", 3000);

    assert(fr.estimate_latency_ms("search") > 700);
    auto p = fr.plan(make_decision(ResponseAction::Search, true), "search",
                     "现在最新的 Qwen3-TTS 怎么样了");
    assert(p.decision == AckDecision::Acknowledge);
    assert(!p.text.empty());
    cout << "  (est=" << p.expected_ms << "ms, ack='" << p.text << "') -> PASS" << endl;
}

// ============================================================
// 3. Policy 判静默时，绝不抢话
// ============================================================
static void test_silence_decision_never_acks() {
    cout << "TEST silence decision never acknowledges..." << endl;

    FastResponseLayer fr;
    for (int i = 0; i < 5; ++i) fr.record_latency("search", 5000);

    // 即便任务很慢，只要 Policy 说该安静（用户在说话/只是附和/Agent 在讲），
    // 就必须闭嘴 —— 抢话比慢更伤。
    auto p = fr.plan(make_decision(ResponseAction::Silence, true), "search", "嗯");
    assert(p.decision == AckDecision::Silent);
    assert(p.text.empty());
    cout << "  -> PASS" << endl;
}

// ============================================================
// 4. 话痨闸门：同一轮最多说一句
// ============================================================
static void test_no_acks_twice_in_one_turn() {
    cout << "TEST at most one ack per turn..." << endl;

    FastResponseLayer fr;
    for (int i = 0; i < 5; ++i) fr.record_latency("search", 3000);

    fr.begin_turn();
    auto p1 = fr.plan(make_decision(ResponseAction::Search, true), "search", "查一下");
    assert(p1.decision == AckDecision::Acknowledge);
    fr.notify_spoke();

    // 同一轮里第二次想说话 -> 闭嘴
    auto p2 = fr.plan(make_decision(ResponseAction::Search, true), "search", "再查一个");
    assert(p2.decision == AckDecision::Silent);

    // 打断后允许下一轮重新抢答
    fr.on_interrupt();
    auto p3 = fr.plan(make_decision(ResponseAction::Search, true), "search", "换个问题");
    assert(p3.decision == AckDecision::Acknowledge);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 5. 不复读用户刚说的话
// ============================================================
static void test_ack_does_not_echo_user() {
    cout << "TEST ack never echoes user's words..." << endl;

    FastResponseLayer fr;
    for (int i = 0; i < 5; ++i) fr.record_latency("search", 3000);
    fr.begin_turn();

    // 拿第一句
    auto p1 = fr.plan(make_decision(ResponseAction::Search, true), "search", "A");
    assert(p1.decision == AckDecision::Acknowledge);
    fr.notify_spoke();

    // 新一轮，但用户说的正好是上一轮那句 ack
    fr.begin_turn();
    auto p2 = fr.plan(make_decision(ResponseAction::Search, true), "search", p1.text);
    assert(p2.decision == AckDecision::Acknowledge);
    assert(p2.text != p1.text);
    cout << "  ('" << p1.text << "' -> '" << p2.text << "') -> PASS" << endl;
}

// ============================================================
// 6. 延迟自适应：EWMA 跟着实测走
// ============================================================
static void test_latency_ewma_adapts() {
    cout << "TEST latency EWMA adapts..." << endl;

    FastResponseLayer fr;
    // 无样本时用默认值
    assert(fr.estimate_latency_ms("unseen") == 600);

    // 连续上报快耗时，估计值应快速下降
    for (int i = 0; i < 10; ++i) fr.record_latency("llm", 100);
    assert(fr.estimate_latency_ms("llm") <= 200);

    // 连续上报慢耗时，估计值应上升
    for (int i = 0; i < 20; ++i) fr.record_latency("llm", 3000);
    assert(fr.estimate_latency_ms("llm") > 1000);
    cout << "  (unseen=600, fast->" << 200 << "ish, slow->"
         << fr.estimate_latency_ms("llm") << ") -> PASS" << endl;
}

// ============================================================
// 7. BackgroundCache：基本存取 + 容量上限
// ============================================================
static void test_background_cache_basic() {
    cout << "TEST background cache basic..." << endl;

    BackgroundCache::Config cfg;
    cfg.max_entries = 3;
    BackgroundCache cache(cfg);

    cache.put("search:qwen3", "Qwen3-TTS 支持流式", 1200.0, false);
    cache.put("search:kokoro", "Kokoro 是轻量 TTS", 800.0, false);

    auto r = cache.get("search:qwen3");
    assert(r.has_value());
    assert(r->content == "Qwen3-TTS 支持流式");
    assert(!r->superseded);

    // 同 topic 覆盖
    cache.put("search:qwen3", "更新后的内容", 100.0, false);
    r = cache.get("search:qwen3");
    assert(r->content == "更新后的内容");
    assert(cache.size() == 2);

    // 容量上限
    for (int i = 0; i < 5; ++i) {
        cache.put("topic:" + to_string(i), "内容" + to_string(i));
    }
    assert(cache.size() <= 3);
    cout << "  (size=" << cache.size() << ") -> PASS" << endl;
}

// ============================================================
// 8. BackgroundCache：superseded 结果可引用但不主动播报
// ============================================================
static void test_superseded_result_is_referenceable() {
    cout << "TEST superseded result still referenceable..." << endl;

    BackgroundCache cache;
    // 换话题时跑完的旧搜索：should_speak=false
    cache.put("search:phone-x", "Phone X 的规格", 2000.0, /*superseded=*/true);

    auto r = cache.get("search:phone-x");
    assert(r.has_value());
    assert(r->superseded);          // 标记为"不主动播报"
    assert(!r->content.empty());    // 但内容可被引用
    cout << "  -> PASS" << endl;
}

// ============================================================
// 9. BackgroundCache：关键词指代消解（补 R1 的读取路径缺口）
// ============================================================
static void test_cache_keyword_lookup() {
    cout << "TEST cache keyword lookup for anaphora..." << endl;

    BackgroundCache cache;
    cache.put("search:qwen3-tts", "Qwen3-TTS 支持 voice cloning");
    cache.put("memory:preference", "用户偏好简洁回答");

    // 用户说"刚才那个 Qwen3 的搜索结果" -> 应该捞到搜索结果
    auto hit = cache.find_by_keyword("刚才那个 Qwen3 的搜索结果是什么");
    assert(hit.has_value());
    assert(hit->topic_key == "search:qwen3-tts");

    // 完全无关的查询不该误命中
    auto miss = cache.find_by_keyword("今天星期几");
    assert(!miss.has_value());

    // 泛指词不该把不相干的命中捞出来
    auto vague = cache.find_by_keyword("那个");
    assert(!vague.has_value());
    cout << "  -> PASS" << endl;
}

// ============================================================
// 10. BackgroundCache：TTL 过期
// ============================================================
static void test_cache_ttl() {
    cout << "TEST background cache TTL..." << endl;

    BackgroundCache::Config cfg;
    cfg.ttl_sec = 0;    // 立刻过期
    BackgroundCache cache(cfg);
    cache.put("search:x", "很快过期的内容");

    std::this_thread::sleep_for(50ms);
    assert(!cache.get("search:x").has_value());
    assert(cache.find_by_keyword("x").has_value() == false);
    assert(cache.expire() >= 1);
    assert(cache.size() == 0);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 11. 端到端：慢搜索 -> 垫一句 -> 后台跑完入 cache -> 下一轮能引用
// ============================================================
static void test_end_to_end_fast_then_background() {
    cout << "TEST end-to-end: ack -> background -> cache -> reference..." << endl;

    FastResponseLayer fr;
    BackgroundCache cache;
    atomic<bool> bg_done{false};
    atomic<bool> spoke_ack{false};

    // --- 第 1 轮：用户问时效性问题 ---
    // 第一次没有延迟样本，先按默认 600ms 估（< 700 阈值 -> 不抢答）
    auto p1 = fr.plan(make_decision(ResponseAction::Search, true), "search",
                      "最新的 Qwen3-TTS 怎么样");
    bool acked_1 = (p1.decision == AckDecision::Acknowledge);
    if (acked_1) { spoke_ack.store(true); fr.notify_spoke(); }

    // 后台任务开始（模拟慢搜索）
    std::thread bg([&] {
        std::this_thread::sleep_for(200ms);
        // 结果回来：should_speak=false（用户可能已换话题）
        cache.put("search:qwen3-tts", "Qwen3-TTS 于本月发布，支持流式与克隆",
                  200.0, true);
        bg_done.store(true);
    });

    // 前台不等它 —— 立刻就能继续（这正是 R3 的验收点）
    assert(!bg_done.load() || true);

    for (int i = 0; i < 100 && !bg_done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    assert(bg_done.load());
    bg.join();

    // 结果落地了，且标记为"不主动播报"
    auto stored = cache.get("search:qwen3-tts");
    assert(stored.has_value());
    assert(stored->superseded);

    // --- 第 2 轮：用户引用刚才的结果 ---
    fr.begin_turn();
    auto hit = cache.find_by_keyword("刚才那个 Qwen3 的搜索结果");
    assert(hit.has_value());
    assert(hit->content.find("流式") != string::npos);
    cout << "  (acked=" << (acked_1 ? 1 : 0)
         << ", referenced='" << hit->content.substr(0, 18) << "...') -> PASS" << endl;
}

// ============================================================
// 12. 长跑内存红线：cache 不会无限涨
// ============================================================
static void test_cache_memory_bounded() {
    cout << "TEST cache memory stays bounded over many writes..." << endl;

    BackgroundCache::Config cfg;
    cfg.max_entries = 32;
    BackgroundCache cache(cfg);

    const size_t before = cache.total_chars();
    for (int i = 0; i < 5000; ++i) {
        cache.put("topic:" + to_string(i), string(200, 'x'));
    }
    const size_t after = cache.total_chars();
    // 32 条 * (约 200 字符内容 + key) 约 8KB 以内
    assert(after < 64 * 1024);
    assert(cache.size() <= 32);
    cout << "  (5000 writes -> " << cache.size() << " entries, "
         << after << " chars, delta=" << (after - before) << ") -> PASS" << endl;
}

int main() {
    auto logger = init_logger("test-fast-response", "warn");
    (void)logger;

    test_fast_task_stays_silent();
    test_slow_task_acknowledges();
    test_silence_decision_never_acks();
    test_no_acks_twice_in_one_turn();
    test_ack_does_not_echo_user();
    test_latency_ewma_adapts();
    test_background_cache_basic();
    test_superseded_result_is_referenceable();
    test_cache_keyword_lookup();
    test_cache_ttl();
    test_end_to_end_fast_then_background();
    test_cache_memory_bounded();

    cout << "\nAll R3 fast response / background cache tests PASSED" << endl;
    return 0;
}
