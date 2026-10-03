// tests/test_turn_admission.cpp
// R1 接线验证：轮次准入决策（意图 + ResponsePolicy + 换话题 + 后台 supersede）
//
// 这一层刻意做成不依赖 LLM/ASR/TTS 的纯逻辑：真实语音链路要模型和音频设备，
// 但"该不该起这一轮"的决策必须能脱离模型单独断言 —— 否则每次验证自然度
// 都要真的说一句话，太慢也太不精确。
#include "orchestrator/turn_admission.hpp"
#include "orchestrator/user_intent.hpp"
#include "orchestrator/response_policy.hpp"
#include "util/log.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace std;
using namespace voice_agent;

namespace {

TurnAdmission make_admission() {
    TurnAdmissionConfig cfg;
    cfg.enable_response_policy = true;
    cfg.enable_topic_supersede = true;
    cfg.agent_speaking = false;
    return TurnAdmission(cfg);
}

}  // namespace

// ---- 1. 附和不该起新轮次 ----
static void test_backchannel_never_starts_turn() {
    cout << "TEST backchannel does not start a turn..." << endl;
    TurnAdmission adm = make_admission();

    for (const char* bc : {"嗯", "嗯嗯", "对", "好的", "OK", "yeah", "收到"}) {
        auto r = adm.evaluate(bc);
        assert(r.intent == UserSpeechIntent::Backchannel);
        assert(!r.should_start_turn);
        assert(!r.should_interrupt);
    }
    cout << "  -> PASS" << endl;
}

// ---- 2. 附和词开头但有实质内容：仍要起轮次 ----
static void test_backchannel_prefix_with_content() {
    cout << "TEST backchannel-prefixed but real content..." << endl;
    TurnAdmission adm = make_admission();

    auto r = adm.evaluate("嗯对了帮我查一下 Qwen3 最新的情况");
    assert(r.intent == UserSpeechIntent::Content);
    assert(r.should_start_turn);
    assert(r.decision.needs_search);
    cout << "  -> PASS" << endl;
}

// ---- 3. 打断：立刻让出话轮，但自己不抢话 ----
static void test_interruption_yields_without_talking() {
    cout << "TEST interruption yields turn but stays quiet..." << endl;
    TurnAdmission adm = make_admission();
    adm.set_agent_speaking(true);

    auto r = adm.evaluate("等一下");
    assert(r.intent == UserSpeechIntent::Interruption);
    assert(r.should_interrupt);
    assert(!r.should_start_turn);   // 说完那两个字就闭嘴，让 Agent 停下

    // 被打断后 Agent 停了，此时同样的话仍应作为新问题起轮次
    adm.set_agent_speaking(false);
    r = adm.evaluate("等一下");
    assert(r.should_interrupt);
    cout << "  -> PASS" << endl;
}

// ---- 4. Agent 正在说话时不抢话 ----
static void test_no_talking_over_agent() {
    cout << "TEST agent speaking -> do not talk over..." << endl;
    TurnAdmission adm = make_admission();
    adm.set_agent_speaking(true);

    auto r = adm.evaluate("我最近真的有好多事情");
    assert(r.intent == UserSpeechIntent::Content);
    assert(!r.should_start_turn);
    assert(r.decision.action == ResponseAction::Silence);
    cout << "  -> PASS" << endl;
}

// ---- 5. 换话题：清 WorkingContext + 标记需要 supersede ----
static void test_topic_change_clears_working_context() {
    cout << "TEST topic change clears transient working context..." << endl;
    TurnAdmission adm = make_admission();

    adm.working().set_topic("SlideTrace");
    adm.working().set_active_project("SlideTrace");
    adm.working().push_entity("时间轴");
    assert(adm.working().active_project().has_value());

    adm.evaluate("我最近在做 talk2agent 的语音链路");
    auto r = adm.evaluate("算了，说点别的");

    assert(r.intent == UserSpeechIntent::TopicChange);
    assert(r.topic_changed);
    assert(r.supersede_stale_tasks);          // 应通知上层 supersede 旧后台任务
    assert(!adm.working().active_project().has_value());  // 临时项已清
    cout << "  -> PASS" << endl;
}

// ---- 6. 连说两次"算了"不算真的换话题 ----
static void test_repeated_topic_change_is_not_a_change() {
    cout << "TEST repeated topic-change words are not a real change..." << endl;
    TurnAdmission adm = make_admission();
    adm.working().set_active_project("SlideTrace");

    auto r1 = adm.evaluate("算了");
    assert(r1.topic_changed);
    adm.working().set_active_project("SlideTrace");   // 重新设上，验证第二次不清

    auto r2 = adm.evaluate("算了");
    assert(!r2.topic_changed);       // 连着说，不算真换
    assert(adm.working().active_project().has_value());
    cout << "  -> PASS" << endl;
}

// ---- 7. 各类输入路由到正确动作 ----
static void test_routing_decisions() {
    cout << "TEST routing decisions..." << endl;
    TurnAdmission adm = make_admission();

    // 时效性 -> Search
    auto r = adm.evaluate("现在最新的 Qwen3-TTS 怎么样了");
    assert(r.should_start_turn);
    assert(r.decision.action == ResponseAction::Search);
    assert(r.decision.needs_search);
    assert(r.decision.allow_background);

    // 本地操作 -> Agent
    r = adm.evaluate("帮我看一下项目里的 build 配置");
    assert(r.decision.action == ResponseAction::Agent);
    assert(r.decision.needs_agent);

    // 个人上下文 -> Memory
    r = adm.evaluate("我之前跟你说过我的偏好了吧");
    assert(r.decision.needs_memory);

    // 复杂推理 -> Deep
    r = adm.evaluate("帮我对比一下 Vulkan 和 DirectML 的架构差异");
    assert(r.decision.action == ResponseAction::DeepReasoning);
    assert(r.decision.tier == ModelTier::Deep);

    // 简单问题 -> 不烧大模型
    r = adm.evaluate("现在几点");
    assert(r.decision.tier == ModelTier::Fast);
    cout << "  -> PASS" << endl;
}

// ---- 8. turn_id 单调递增，供事件串联 ----
static void test_turn_id_monotonic() {
    cout << "TEST turn id increases monotonically..." << endl;
    TurnAdmission adm = make_admission();

    uint64_t prev = adm.current_turn_id();
    for (int i = 0; i < 5; ++i) {
        adm.evaluate("帮我查一下第 " + to_string(i) + " 个东西");
        uint64_t cur = adm.current_turn_id();
        assert(cur > prev);
        prev = cur;
    }
    // 静默的轮次不消耗 turn_id
    uint64_t before = adm.current_turn_id();
    adm.evaluate("嗯");
    assert(adm.current_turn_id() == before);
    cout << "  -> PASS" << endl;
}

// ---- 9. 静默窗口 ----
static void test_silence_window() {
    cout << "TEST forced silence window..." << endl;
    TurnAdmission adm = make_admission();

    adm.policy().set_silence_window_ms(500);
    auto r = adm.evaluate("今天天气不错");
    assert(!r.should_start_turn);
    assert(r.decision.action == ResponseAction::Silence);

    adm.policy().clear_silence_window();
    r = adm.evaluate("今天天气不错");
    assert(r.should_start_turn);
    cout << "  -> PASS" << endl;
}

// ---- 10. 续说：不打断，等 Agent 说完 ----
static void test_continuation_does_not_interrupt() {
    cout << "TEST continuation waits instead of interrupting..." << endl;
    TurnAdmission adm = make_admission();
    adm.set_agent_speaking(true);

    auto r = adm.evaluate("继续");
    assert(r.intent == UserSpeechIntent::Continuation);
    assert(!r.should_interrupt);      // 别抢话，让它讲完
    cout << "  -> PASS" << endl;
}

int main() {
    auto logger = init_logger("test-turn-admission", "warn");
    (void)logger;

    test_backchannel_never_starts_turn();
    test_backchannel_prefix_with_content();
    test_interruption_yields_without_talking();
    test_no_talking_over_agent();
    test_topic_change_clears_working_context();
    test_repeated_topic_change_is_not_a_change();
    test_routing_decisions();
    test_turn_id_monotonic();
    test_silence_window();
    test_continuation_does_not_interrupt();

    cout << "\nAll R1 turn admission tests PASSED" << endl;
    return 0;
}
