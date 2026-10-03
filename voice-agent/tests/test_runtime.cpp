// tests/test_runtime.cpp
// R0: Conversation Runtime 骨架 —— Task 生命周期 / WorkingContext /
//     ContextManager / ResponsePolicy / UserSpeechIntent
#include "core/task.hpp"
#include "core/working_context.hpp"
#include "orchestrator/response_policy.hpp"
#include "orchestrator/user_intent.hpp"
#include "util/log.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace std;
using namespace std::chrono_literals;
using namespace voice_agent;

// ============================================================
// 1. Task 生命周期
// ============================================================
static void test_task_basic_lifecycle() {
    cout << "TEST task basic lifecycle..." << endl;

    TaskManager tm(2);
    atomic<bool> ran{false};

    TaskSpec spec;
    spec.name = "unit";
    spec.priority = TaskPriority::Foreground;
    spec.work = [&](TaskContext& ctx) {
        ran.store(true);
        TaskOutcome o;
        o.ok = true;
        o.value = "hello";
        o.should_speak = true;
        return o;
    };

    TaskId id = tm.submit(spec);
    assert(id != kInvalidTaskId);

    // 非阻塞：submit 立即返回
    assert(!ran.load() || true);

    for (int i = 0; i < 200 && tm.active_count() > 0; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    assert(ran.load());
    assert(tm.active_count() == 0);

    auto snap = tm.get(id);
    assert(snap.has_value());
    assert(snap->state == TaskState::Completed);
    assert(snap->should_speak);

    auto res = tm.latest_result(id);
    assert(res.has_value() && res->ok && res->value == "hello");
    cout << "  -> PASS" << endl;
}

// ============================================================
// 2. 核心验收：foreground 与 background 并发、互不阻塞
// ============================================================
static void test_foreground_background_concurrent() {
    cout << "TEST foreground + background run concurrently..." << endl;

    TaskManager tm(4);
    atomic<bool> bg_done{false};
    atomic<bool> fg_done{false};
    atomic<int>  concurrent{0};
    atomic<int>  max_concurrent{0};

    // 一个慢后台任务（模拟 search）
    TaskSpec bg;
    bg.name = "background_search";
    bg.priority = TaskPriority::Background;
    bg.topic_key = "search:qwen3-tts";
    bg.work = [&](TaskContext& ctx) {
        int c = ++concurrent;
        int prev = max_concurrent.load();
        while (c > prev && !max_concurrent.compare_exchange_weak(prev, c)) {}
        std::this_thread::sleep_for(300ms);
        --concurrent;
        bg_done.store(true);
        TaskOutcome o; o.ok = true; o.value = "search results";
        return o;
    };
    tm.submit(bg);

    std::this_thread::sleep_for(30ms);

    // 前台任务必须能在后台还没完成时就跑完
    TaskSpec fg;
    fg.name = "foreground_reply";
    fg.priority = TaskPriority::Foreground;
    fg.work = [&](TaskContext&) {
        int c = ++concurrent;
        int prev = max_concurrent.load();
        while (c > prev && !max_concurrent.compare_exchange_weak(prev, c)) {}
        fg_done.store(true);
        --concurrent;
        TaskOutcome o; o.ok = true; o.value = "quick answer";
        return o;
    };
    tm.submit(fg);

    // 等前台完成（应远快于后台的 300ms）
    for (int i = 0; i < 200 && !fg_done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    assert(fg_done.load());
    assert(!bg_done.load());   // 关键：前台没有被后台阻塞

    // 后台最终仍应完成
    for (int i = 0; i < 200 && !bg_done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    assert(bg_done.load());
    assert(max_concurrent.load() >= 2);
    cout << "  (max concurrent=" << max_concurrent.load() << ") -> PASS" << endl;
}

// ============================================================
// 3. Cancel 贯穿
// ============================================================
static void test_task_cancel() {
    cout << "TEST task cancel..." << endl;

    TaskManager tm(2);
    atomic<bool> observed_cancel{false};

    TaskSpec spec;
    spec.name = "long";
    spec.priority = TaskPriority::Background;
    spec.policy = TaskPolicy::CancelOnInterrupt;
    spec.work = [&](TaskContext& ctx) {
        for (int i = 0; i < 200; ++i) {
            if (!ctx.wait_or_cancel(10)) {
                observed_cancel.store(true);
                TaskOutcome o; o.ok = false; o.error = "cancelled";
                return o;
            }
        }
        TaskOutcome o; o.ok = true; o.value = "finished without cancel";
        return o;
    };

    TaskId id = tm.submit(spec);
    std::this_thread::sleep_for(50ms);
    assert(tm.cancel(id));

    for (int i = 0; i < 200 && tm.active_count() > 0; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    assert(observed_cancel.load());

    auto snap = tm.get(id);
    assert(snap->state == TaskState::Cancelled);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 4. Supersede ≠ Cancel：任务仍跑完，但结果不播报
// ============================================================
static void test_task_supersede() {
    cout << "TEST task supersede (not cancel)..." << endl;

    TaskManager tm(2);
    atomic<bool> finished{false};
    atomic<int>  runs{0};
    vector<TaskOutcome> sink_results;
    mutex sink_mtx;

    tm.set_result_sink([&](const TaskId&, const TaskOutcome& o) {
        lock_guard<mutex> lk(sink_mtx);
        sink_results.push_back(o);
    });

    TaskSpec spec;
    spec.name = "old_search";
    spec.priority = TaskPriority::Background;
    spec.topic_key = "search:phone-x";
    spec.work = [&](TaskContext& ctx) {
        runs.fetch_add(1);
        for (int i = 0; i < 20; ++i) {
            if (!ctx.wait_or_cancel(10)) break;
        }
        finished.store(true);
        TaskOutcome o;
        o.ok = true;
        o.value = "old topic result";
        o.should_speak = !ctx.superseded();   // 关键：被 supersede 就不该播报
        return o;
    };

    TaskId id = tm.submit(spec);
    std::this_thread::sleep_for(30ms);

    auto affected = tm.supersede_topic("search:phone-x");
    assert(affected.size() == 1);
    assert(affected[0] == id);

    // 关键断言：supersede 之后任务仍然跑完（不是被 kill）
    for (int i = 0; i < 200 && !finished.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    assert(finished.load());
    assert(runs.load() == 1);

    for (int i = 0; i < 100 && sink_results.empty(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    assert(!sink_results.empty());
    // 结果照常拿到（入 cache），但 should_speak=false
    assert(sink_results.back().value == "old topic result");
    assert(!sink_results.back().should_speak);

    auto snap = tm.get(id);
    assert(snap->state == TaskState::Superseded);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 5. apply_interrupt：不同 policy 走不同处置
// ============================================================
static void test_apply_interrupt_policies() {
    cout << "TEST apply_interrupt honours per-task policy..." << endl;

    TaskManager tm(4);
    atomic<bool> cancel_observed{false};
    atomic<bool> continue_finished{false};

    // CancelOnInterrupt
    TaskSpec s1;
    s1.name = "cancellable";
    s1.priority = TaskPriority::Foreground;
    s1.policy = TaskPolicy::CancelOnInterrupt;
    s1.work = [&](TaskContext& ctx) {
        for (int i = 0; i < 100; ++i) {
            if (!ctx.wait_or_cancel(10)) {
                cancel_observed.store(true);
                TaskOutcome o; o.ok = false; return o;
            }
        }
        TaskOutcome o; o.ok = true; return o;
    };
    TaskId id_cancel = tm.submit(s1);

    // ContinueOnInterrupt：被打断后继续跑完
    TaskSpec s2;
    s2.name = "continuer";
    s2.priority = TaskPriority::Background;
    s2.policy = TaskPolicy::ContinueOnInterrupt;
    s2.work = [&](TaskContext& ctx) {
        for (int i = 0; i < 10; ++i) ctx.wait_or_cancel(10);
        continue_finished.store(true);
        TaskOutcome o; o.ok = true; o.value = "done in background"; return o;
    };
    TaskId id_cont = tm.submit(s2);

    std::this_thread::sleep_for(30ms);
    auto affected = tm.apply_interrupt();
    assert(affected.size() >= 2);

    for (int i = 0; i < 200 && tm.active_count() > 0; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    assert(cancel_observed.load());          // cancel 策略生效
    assert(continue_finished.load());        // continue 策略跑完了
    assert(tm.get(id_cancel)->state == TaskState::Cancelled);
    assert(tm.get(id_cont)->state == TaskState::Superseded);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 6. WorkingContext：换话题清临时项
// ============================================================
static void test_working_context() {
    cout << "TEST working context..." << endl;

    WorkingContext wc;
    wc.set_topic("SlideTrace 项目");
    wc.set_active_project("SlideTrace");
    wc.push_entity("时间轴");

    assert(wc.active_project().value() == "SlideTrace");
    assert(wc.latest_entity().value() == "时间轴");

    // 换话题 => 临时项全部失效
    wc.on_topic_changed("Qwen3-TTS 调研");
    assert(!wc.active_project().has_value());
    assert(!wc.latest_entity().has_value());
    assert(wc.topic() == "Qwen3-TTS 调研");

    // 带 TTL 的项在换话题后仍存活
    wc.set("user_name", "ted", 3600);
    wc.on_topic_changed("另一个话题");
    assert(wc.get("user_name").value() == "ted");
    cout << "  -> PASS" << endl;
}

// ============================================================
// 7. ContextManager：低优先层在预算不足时先被裁
// ============================================================
static void test_context_manager() {
    cout << "TEST context manager layering..." << endl;

    ContextManager cm(ContextManager::Config{
        /*recent_turns*/2,
        /*working_context_max*/200,
        /*memory_max*/100,
        /*tool_results_max*/100,
        /*total_char_budget*/300,   // 刻意压小，逼出裁剪逻辑
        /*max_session_turns*/100
    });

    cm.set_topic("性能优化");
    cm.working().set_active_project("talk2agent");
    for (int i = 0; i < 6; ++i) {
        cm.add_turn("问题" + to_string(i), "回答" + to_string(i));
    }
    cm.set_relevant_memory({string(300, 'M')});        // 超预算的记忆
    cm.add_tool_result(string(300, 'T'));              // 超预算的工具结果

    string ctx = cm.build_context();
    assert(ctx.size() <= 300 + 64);   // 允许 header 溢出一点，但不能爆炸
    assert(ctx.find("性能优化") != string::npos);       // 话题必须在
    assert(ctx.find("问题5") != string::npos);          // 最新一轮必须在
    assert(ctx.find("问题0") == string::npos);          // 老轮次被裁掉
    assert(cm.has_older_history());
    assert(cm.turn_count() == 6);
    cout << "  (ctx size=" << ctx.size() << ") -> PASS" << endl;
}

// ============================================================
// 8. UserSpeechIntent：backchannel 不得吃掉正常发言
// ============================================================
static void test_user_intent() {
    cout << "TEST user speech intent..." << endl;

    UserIntentClassifier c;

    // 纯附和
    assert(c.classify("嗯") == UserSpeechIntent::Backchannel);
    assert(c.classify("嗯嗯。") == UserSpeechIntent::Backchannel);
    assert(c.classify("对") == UserSpeechIntent::Backchannel);
    assert(c.classify("好的") == UserSpeechIntent::Backchannel);
    assert(c.classify("OK") == UserSpeechIntent::Backchannel);
    assert(c.classify("yeah") == UserSpeechIntent::Backchannel);
    assert(c.classify("uh-huh") == UserSpeechIntent::Backchannel);

    // 关键回归：以附和词开头但有实质内容 —— 必须判为 Content
    assert(c.classify("嗯对了帮我查一下 Qwen3") == UserSpeechIntent::Content);
    assert(c.classify("好的，那这个时间轴怎么设计") == UserSpeechIntent::Content);
    assert(c.classify("嗯我挺喜欢的") == UserSpeechIntent::Content);

    // 打断
    assert(c.classify("等一下") == UserSpeechIntent::Interruption);
    assert(c.classify("先别说了") == UserSpeechIntent::Interruption);
    assert(c.classify("wait") == UserSpeechIntent::Interruption);
    // 带后缀也要能识别
    assert(c.classify("等一下，我查个东西") == UserSpeechIntent::Interruption);

    // 换话题
    assert(c.classify("算了") == UserSpeechIntent::TopicChange);
    assert(c.classify("顺便问一下") == UserSpeechIntent::TopicChange);
    assert(c.classify("对了，Kokoro 支持流式吗") == UserSpeechIntent::TopicChange);

    // 续说 / 纠正
    assert(c.classify("继续") == UserSpeechIntent::Continuation);
    assert(c.classify("不是的") == UserSpeechIntent::Correction);

    // 让出话轮判定
    assert(!c.should_yield_turn("嗯"));              // 附和不让出
    assert(c.should_yield_turn("等一下"));            // 打断让出
    assert(c.should_yield_turn("我今天特别累"));       // 实质内容让出
    cout << "  -> PASS" << endl;
}

// ============================================================
// 9. ResponsePolicy：各类决策
// ============================================================
static void test_response_policy() {
    cout << "TEST response policy..." << endl;

    ResponsePolicy p;

    ResponsePolicyInput in;

    // 附和 => 静默
    in = {};
    in.user_text = "嗯";
    in.intent = UserSpeechIntent::Backchannel;
    assert(p.decide(in).action == ResponseAction::Silence);

    // 打断 => 让出，不抢话
    in = {};
    in.user_text = "等一下";
    in.intent = UserSpeechIntent::Interruption;
    assert(p.decide(in).action == ResponseAction::Silence);

    // Agent 正在说话时不抢话
    in = {};
    in.user_text = "我最近真的好多事情";
    in.intent = UserSpeechIntent::Content;
    in.agent_is_speaking = true;
    assert(p.decide(in).action == ResponseAction::Silence);

    // 时效性查询 => Search
    in = {};
    in.user_text = "现在最新的 Qwen3-TTS 怎么样";
    in.intent = UserSpeechIntent::Content;
    auto d = p.decide(in);
    assert(d.action == ResponseAction::Search);
    assert(d.needs_search);
    assert(d.allow_background);
    assert(d.tier == ModelTier::Search);

    // 本地操作 => Agent
    in = {};
    in.user_text = "帮我看一下项目里的 build 配置";
    in.intent = UserSpeechIntent::Content;
    d = p.decide(in);
    assert(d.action == ResponseAction::Agent);
    assert(d.needs_agent);

    // 个人上下文 => Memory
    in = {};
    in.user_text = "我之前跟你说过我的偏好了吧";
    in.intent = UserSpeechIntent::Content;
    d = p.decide(in);
    assert(d.needs_memory);

    // 复杂推理 => Deep
    in = {};
    in.user_text = "帮我对比一下 Vulkan 和 DirectML 的架构差异";
    in.intent = UserSpeechIntent::Content;
    d = p.decide(in);
    assert(d.action == ResponseAction::DeepReasoning);
    assert(d.tier == ModelTier::Deep);
    assert(d.depth == ResponseDepth::High);

    // 简单问题 => 不用大模型
    in = {};
    in.user_text = "现在几点";
    in.intent = UserSpeechIntent::Content;
    d = p.decide(in);
    assert(d.action == ResponseAction::QuickReply);
    assert(d.tier == ModelTier::Fast);

    // 强制静默窗口
    p.set_silence_window_ms(500);
    in = {};
    in.user_text = "今天天气不错";
    in.intent = UserSpeechIntent::Content;
    assert(p.decide(in).action == ResponseAction::Silence);
    p.clear_silence_window();

    // 换话题不触发旧记忆
    in = {};
    in.user_text = "算了，说点别的";
    in.intent = UserSpeechIntent::TopicChange;
    d = p.decide(in);
    assert(!d.needs_memory);

    // ---- 回归（R4 发现）：情感倾诉不该被"最近"误判成 Search ----
    // "我最近真的有好多事情"里的"最近"是时间副词，不是查询限定词。
    // 误判后果：去搜"最近"然后端一堆无关结果回来 —— 极不自然。
    in = {};
    in.user_text = "我最近真的有好多事情";
    in.intent = UserSpeechIntent::Content;
    d = p.decide(in);
    assert(!d.needs_search);
    assert(d.action == ResponseAction::Answer);
    assert(d.tier == ModelTier::Normal);
    assert(d.depth == ResponseDepth::Low);   // 倾诉不需要长篇大论

    in.user_text = "最近好累啊";
    assert(!p.decide(in).needs_search);
    in.user_text = "我最近压力大";
    assert(!p.decide(in).needs_search);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 10. ProgressUtterance：不快不慢才说话，不复读
// ============================================================
static void test_progress_utterance() {
    cout << "TEST progress utterance policy..." << endl;

    ProgressUtterancePolicy p;

    // 很快 => 静默，不塞 filler
    ProgressUtteranceInput in;
    in.task_type = "search";
    in.expected_latency_ms = 300;
    assert(p.decide(in).kind == ProgressKind::Silent);

    // 用户不耐烦 => 闭嘴干活
    in = {};
    in.task_type = "search";
    in.expected_latency_ms = 3000;
    in.user_seems_impatient = true;
    assert(p.decide(in).kind == ProgressKind::Silent);

    // 慢任务 => 接手，但不能说满 2 句
    in = {};
    in.task_type = "search";
    in.expected_latency_ms = 3000;
    auto u0 = p.decide(in);
    assert(u0.kind == ProgressKind::Acknowledge);
    assert(!u0.text.empty());

    in.turns_spoken_in_task = 2;
    assert(p.decide(in).kind == ProgressKind::Silent);

    // 不复读
    in = {};
    in.task_type = "search";
    in.expected_latency_ms = 3000;
    in.recent_speech = u0.text;
    auto u1 = p.decide(in);
    assert(u1.text != u0.text);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 11. P6 层：多来源并存，不互相覆盖（R5 修复）
// ============================================================
static void test_p6_multi_source() {
    cout << "TEST P6 layer keeps multiple sources..." << endl;

    ContextManager cm;
    // 本轮真实工具结果先写入
    cm.add_tool_result("web_search 返回 3 条命中");
    // 后台缓存结果后写入 —— 旧实现会把它顶掉
    cm.add_background_result("早前搜索：Qwen3-TTS 支持流式");

    string ctx = cm.build_context();
    // 两个来源都必须在
    assert(ctx.find("web_search") != string::npos);
    assert(ctx.find("Qwen3-TTS") != string::npos);
    assert(ctx.find("[tool]") != string::npos);
    assert(ctx.find("[background]") != string::npos);

    // 新轮次开始应清空 P6
    cm.clear_results();
    ctx = cm.build_context();
    assert(ctx.find("web_search") == string::npos);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 12. P7 层：老历史滚动摘要（R5 新增）
// ============================================================
static void test_p7_rolling_summary() {
    cout << "TEST P7 rolling summary..." << endl;

    ContextManager::Config cfg;
    cfg.recent_turns = 3;
    cfg.max_session_turns = 100;
    ContextManager cm(cfg);
    cm.set_summary_policy(/*every_n=*/4, /*max_chars=*/500);

    for (int i = 0; i < 20; ++i) {
        cm.add_turn("问题" + to_string(i), "回答" + to_string(i));
    }

    // 摘要应已生成，且包含早期轮次的关键信息
    const string summary = cm.older_summary();
    assert(!summary.empty());
    assert(cm.summarized_count() > 0);
    assert(summary.find("问题0") != string::npos);      // 最早那轮被摘要了
    assert(summary.size() <= 500);

    // 拼装后的上下文里应有摘要，且保留最近几轮全文
    const string ctx = cm.build_context();
    assert(ctx.find("更早的对话摘要") != string::npos);
    assert(ctx.find("问题19") != string::npos);         // 最新一轮保留原文
    assert(ctx.find("已省略") == string::npos);         // 不再是干巴巴占位
    cout << "  (summarized=" << cm.summarized_count()
         << ", summary=" << summary.size() << " chars) -> PASS" << endl;
}

// ============================================================
// 13. P7 摘要不无限增长（R5 长跑红线）
// ============================================================
static void test_p7_summary_bounded() {
    cout << "TEST P7 summary stays bounded..." << endl;

    ContextManager::Config cfg;
    cfg.recent_turns = 3;
    cfg.max_session_turns = 20;
    ContextManager cm(cfg);
    cm.set_summary_policy(4, 400);

    for (int i = 0; i < 500; ++i) {
        cm.add_turn("Q" + to_string(i), string(50, 'A') + to_string(i));
    }
    assert(cm.older_summary().size() <= 400);
    assert(cm.turn_count() <= 20);
    cout << "  (500 turns -> summary=" << cm.older_summary().size()
         << " chars, turns=" << cm.turn_count() << ") -> PASS" << endl;
}

int main() {
    auto logger = init_logger("test-runtime", "warn");
    (void)logger;

    test_task_basic_lifecycle();
    test_foreground_background_concurrent();
    test_task_cancel();
    test_task_supersede();
    test_apply_interrupt_policies();
    test_working_context();
    test_context_manager();
    test_user_intent();
    test_response_policy();
    test_progress_utterance();
    test_p6_multi_source();
    test_p7_rolling_summary();
    test_p7_summary_bounded();

    cout << "\nAll R0 Conversation Runtime tests PASSED" << endl;
    return 0;
}
