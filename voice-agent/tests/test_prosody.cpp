// tests/test_prosody.cpp
// R6: ProsodyPlanner（韵律规划）+ ResponsePlan（响应修订）+ TTS Adapter（韵律映射）
#include "tts/prosody.hpp"
#include "tts/response_plan.hpp"
#include "tts/tts_adapter.hpp"
#include "util/log.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

using namespace std;
using namespace voice_agent;

namespace {
bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }
}

// ============================================================
// 1. 句级切分
// ============================================================
static void test_sentence_split() {
    cout << "TEST sentence split..." << endl;

    auto s = ProsodyPlanner::split_sentences("我觉得这里有两个问题。第一是延迟，第二是稳定性。");
    assert(s.size() == 3);
    assert(s[0] == "我觉得这里有两个问题。");
    assert(s[1] == "第一是延迟，");   // 逗号是次级标点，不切
    assert(s[2] == "第二是稳定性。");

    // 连续标点并入同一段
    s = ProsodyPlanner::split_sentences("真的吗？！太好了。");
    assert(s.size() == 2);
    assert(s[0] == "真的吗？！");

    // 英文标点
    s = ProsodyPlanner::split_sentences("Is it fast? Yes, very fast!");
    assert(s.size() == 2);

    // 纯标点段必须被丢弃（否则会合成一段没有内容的音频）
    s = ProsodyPlanner::split_sentences("好。。。行吧。");
    assert(s.size() == 2);
    assert(s[0] == "好。。。");

    // 超长无标点文本：按 max_chars 硬切
    s = ProsodyPlanner::split_sentences(string(300, 'a'), 100);
    assert(s.size() == 3);

    // 空输入
    assert(ProsodyPlanner::split_sentences("").empty());
    cout << "  -> PASS" << endl;
}

// ============================================================
// 2. 情绪 → 韵律：小幅偏移，不是覆盖
// ============================================================
static void test_emotion_prosody() {
    cout << "TEST emotion to prosody mapping..." << endl;

    Prosody base;   // 中性
    Prosody happy = prosody_for_emotion("happy", base);
    Prosody sad = prosody_for_emotion("sad", base);
    Prosody calm = prosody_for_emotion("calm", base);

    // 开心：能量高、语速快
    assert(happy.energy > base.energy);
    assert(happy.pace > base.pace);

    // 难过：能量低、语速慢、停顿多
    assert(sad.energy < base.energy);
    assert(sad.pace < base.pace);
    assert(sad.pause_density > base.pause_density);

    // 平静：能量低但温暖高（这正是"平静讨论"的听感）
    assert(calm.energy < base.energy);
    assert(calm.warmth > base.warmth);

    // 关键：情绪是染色不是换人 —— 偏移幅度必须有限
    assert(std::fabs(happy.energy - base.energy) < 0.35f);
    assert(std::fabs(sad.pace - base.pace) < 0.35f);

    // 用户不耐烦：能量和语速都要降 —— 再热情就是火上浇油
    Prosody irritated = prosody_for_emotion("user_irritated", base);
    assert(irritated.energy < base.energy);
    assert(irritated.pace < base.pace);
    assert(irritated.urgency < base.urgency);

    // 未知情绪 / 空情绪 => 完全不动
    assert(prosody_for_emotion("", base) == base);
    assert(prosody_for_emotion("nonexistent_emotion", base) == base);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 3. 情绪强度插值：半吊子情绪比没情绪更像机器人
// ============================================================
static void test_emotion_intensity() {
    cout << "TEST emotion intensity interpolation..." << endl;

    ProsodyPlanner pp;

    // intensity = 0 => 纯中性
    auto seg0 = pp.plan_segment("好的。", "excited", 0.0f);
    assert(near(seg0.prosody.energy, 0.5f));
    assert(near(seg0.prosody.pace, 0.5f));

    // intensity = 1 => 完整情绪
    auto seg1 = pp.plan_segment("好的。", "excited", 1.0f);
    assert(seg1.prosody.energy > seg0.prosody.energy);

    // intensity = 0.5 => 居中
    auto seg05 = pp.plan_segment("好的。", "excited", 0.5f);
    assert(seg05.prosody.energy > seg0.prosody.energy);
    assert(seg05.prosody.energy < seg1.prosody.energy);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 4. 轮次首尾的韵律处理
// ============================================================
static void test_turn_head_tail() {
    cout << "TEST turn head/tail prosody..." << endl;

    ProsodyPlanner pp;
    auto segs = pp.plan("第一句。第二句。第三句。");

    assert(segs.size() == 3);
    // 轮次开头要稍停，让用户确认"这是在回答我"
    assert(segs.front().pause_before_ms > 0);
    // 轮次末尾要留白，否则下一句紧贴上来像说了两件事
    assert(segs.back().pause_after_ms >= 180);
    // 中间段不额外加头停顿
    assert(segs[1].pause_before_ms == 0);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 5. 时长估计（Response Planner 依赖它判断"来得及吗"）
// ============================================================
static void test_duration_estimate() {
    cout << "TEST duration estimation..." << endl;

    ProsodyPlanner pp;
    Prosody slow;
    slow.pace = 0.0f;
    Prosody fast;
    fast.pace = 1.0f;

    const int d_slow = pp.estimate_duration_ms("这是一段测试文本", slow);
    const int d_fast = pp.estimate_duration_ms("这是一段测试文本", fast);
    assert(d_slow > d_fast);         // 语速慢 => 估时更长
    assert(d_slow > 0);

    // 时长估算要随文本变长而变长
    assert(pp.estimate_duration_ms(string(100, 'a')) >
           pp.estimate_duration_ms(string(10, 'a')));
    cout << "  (slow=" << d_slow << "ms fast=" << d_fast << "ms) -> PASS" << endl;
}

// ============================================================
// 6. 韵律 → 引擎参数映射
// ============================================================
static void test_prosody_to_engine_params() {
    cout << "TEST prosody to engine params..." << endl;

    Prosody calm;
    calm.pace = 0.1f; calm.energy = 0.2f; calm.certainty = 0.8f;
    Prosody urgent;
    urgent.pace = 0.9f; urgent.energy = 0.9f; urgent.urgency = 0.9f;
    urgent.certainty = 0.2f;

    auto m_calm = map_prosody_for_model(calm);
    auto m_urgent = map_prosody_for_model(urgent);
    assert(m_urgent.speed > m_calm.speed);
    // 范围钳制：再急也不能超过 1.8（听感反而变差）
    assert(m_urgent.speed <= 1.8f);
    assert(m_calm.speed >= 0.6f);

    auto s_calm = map_prosody_for_sapi(calm);
    auto s_urgent = map_prosody_for_sapi(urgent);
    // SAPI 能表达音量：能量高的应该更响
    assert(s_urgent.volume > s_calm.volume);
    assert(s_urgent.speed > s_calm.speed);
    // 音调变化必须很小 —— 超过 ±5% 就阴阳怪气
    assert(s_urgent.pitch >= 0.95f && s_urgent.pitch <= 1.05f);
    // 音量也不能太离谱
    assert(s_urgent.volume <= 1.3f);

    // 不确定的句子停顿更久
    assert(s_urgent.pause_ms >= 0);
    cout << "  (calm speed=" << s_calm.speed << " vol=" << s_calm.volume
         << ", urgent speed=" << s_urgent.speed << " vol=" << s_urgent.volume
         << ") -> PASS" << endl;
}

// ============================================================
// 7. ResponsePlan 基本生命周期
// ============================================================
static void test_response_plan_basic() {
    cout << "TEST response plan lifecycle..." << endl;

    ProsodyPlanner pp;
    ResponsePlan plan;

    plan.append(pp.plan("第一句。第二句。"));
    assert(plan.pending_count() == 2);

    auto s0 = plan.next_to_synthesize();
    assert(s0.has_value());
    assert(s0->text == "第一句。");
    plan.mark_synthesized(0);
    plan.mark_playing(0);
    assert(plan.playing_index() == 0);

    plan.mark_played(0);
    assert(plan.played_count() == 1);

    auto s1 = plan.next_to_synthesize();
    assert(s1.has_value() && s1->text == "第二句。");
    plan.mark_synthesized(1);
    plan.mark_playing(1);
    plan.mark_played(1);

    assert(plan.finished());
    assert(plan.next_to_synthesize() == std::nullopt);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 8. Response Revision：已播不可改，未播可丢
// ============================================================
static void test_response_revision() {
    cout << "TEST response revision: played stays, unplayed dropped..." << endl;

    ProsodyPlanner pp;
    ResponsePlan plan;
    plan.append(pp.plan("第一句。第二句。第三句。第四句。"));

    // 播完前两句
    plan.mark_synthesized(0); plan.mark_playing(0); plan.mark_played(0);
    plan.mark_synthesized(1); plan.mark_playing(1); plan.mark_played(1);

    // 模型改了主意：丢弃第 2 句之后所有未播内容
    const size_t dropped = plan.discard_unplayed_from(2);
    assert(dropped == 2);          // 第 3、4 句
    assert(plan.played_count() == 2);   // 前两句不受影响

    // 重新规划后续内容
    plan.append(pp.plan("其实我想说的是另一件事。"));
    auto s = plan.next_to_synthesize();
    assert(s.has_value());
    // 游标应该跳过已丢弃的段，落在新追加的段上
    assert(s->text.find("其实我想说") != string::npos);

    auto st = plan.stats();
    assert(st.discarded == 2);
    assert(st.revised_rounds == 1);
    cout << "  (dropped=" << dropped << ", played=" << plan.played_count()
         << ") -> PASS" << endl;
}

// ============================================================
// 9. 正在播的段不能被丢弃
// ============================================================
static void test_cannot_discard_playing() {
    cout << "TEST cannot discard currently playing segment..." << endl;

    ProsodyPlanner pp;
    ResponsePlan plan;
    plan.append(pp.plan("第一句。第二句。第三句。"));
    plan.mark_synthesized(0); plan.mark_playing(0);

    const size_t dropped = plan.discard_all_unplayed();
    // 第 1 句在播，不能丢；第 2、3 句可丢
    assert(dropped == 2);
    assert(plan.playing_index() == 0);

    plan.mark_played(0);
    assert(plan.played_count() == 1);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 10. 丢弃后仍可继续推进，不会卡住
// ============================================================
static void test_no_stall_after_discard() {
    cout << "TEST plan does not stall after discard..." << endl;

    ProsodyPlanner pp;
    ResponsePlan plan;
    plan.append(pp.plan("A。B。C。"));
    plan.discard_unplayed_from(0);     // 全丢
    assert(plan.pending_count() == 0);
    assert(plan.finished());
    assert(plan.next_to_synthesize() == std::nullopt);

    // 丢弃后再追加必须能被取到
    plan.append(pp.plan("新内容。"));
    assert(!plan.finished());
    auto s = plan.next_to_synthesize();
    assert(s.has_value() && s->text == "新内容。");
    cout << "  -> PASS" << endl;
}

// ============================================================
// 11. 端到端：token 流 → 切段 → 韵律 → ResponsePlan
//     模拟 Orchestrator 的 feed_speech_text_ 逻辑
// ============================================================
static void test_token_stream_to_segments() {
    cout << "TEST token stream to planned segments..." << endl;

    ProsodyPlanner pp;
    ResponsePlan plan;
    std::string pending;

    // 模拟 LLM 逐 token 吐出
    const char* tokens[] = {"我", "觉得", "这里", "有两个", "问题",
                            "。", "第一", "是", "延迟", "。", "第二",
                            "是", "稳定性", ""};
    std::vector<std::string> synthesized;

    for (const char* tok : tokens) {
        pending += tok;
        auto sents = ProsodyPlanner::split_sentences(pending);
        if (sents.empty()) continue;
        size_t consumed = 0;
        for (const auto& s : sents) consumed += s.size();
        pending.erase(0, consumed);

        std::vector<SpeechSegment> segs;
        for (size_t i = 0; i < sents.size(); ++i) {
            segs.push_back(pp.plan_segment(sents[i], "calm", 0.5f));
        }
        plan.append(segs);

        while (auto s = plan.next_to_synthesize()) {
            synthesized.push_back(s->text);
            const size_t idx = synthesized.size() - 1;
            plan.mark_synthesized(idx);
            plan.mark_playing(idx);
            plan.mark_played(idx);
        }
    }
    // 残留（无标点的尾句）由 flush 补上
    if (!pending.empty()) {
        plan.append({pp.plan_segment(pending, "calm", 0.5f)});
        if (auto s = plan.next_to_synthesize()) {
            synthesized.push_back(s->text);
            const size_t idx = synthesized.size() - 1;
            plan.mark_synthesized(idx);
            plan.mark_playing(idx);
            plan.mark_played(idx);
        }
    }

    assert(synthesized.size() == 3);
    assert(synthesized[0] == "我觉得这里有两个问题。");
    assert(synthesized[1] == "第一是延迟。");
    assert(synthesized[2] == "第二是稳定性");
    assert(plan.finished());

    auto st = plan.stats();
    assert(st.appended == 3);
    assert(st.played == 3);
    assert(st.discarded == 0);
    cout << "  (" << synthesized.size() << " segments) -> PASS" << endl;
}

// ============================================================
// 12. 情绪对韵律的实际影响（端到端视角）
// ============================================================
static void test_emotion_reaches_segments() {
    cout << "TEST emotion reaches final segments..." << endl;

    ProsodyPlanner pp;
    const std::vector<SpeechSegment> calm = pp.plan("好的。", "calm", 0.8f);
    const std::vector<SpeechSegment> excited = pp.plan("好的。", "excited", 0.8f);

    assert(calm.size() == 1 && excited.size() == 1);
    // 兴奋的段语速更快 -> 引擎语速倍率更高
    assert(ProsodyPlanner::rate_from_prosody(excited[0].prosody) >
           ProsodyPlanner::rate_from_prosody(calm[0].prosody));
    // 平静的段停顿更明显
    assert(calm[0].pause_after_ms > excited[0].pause_after_ms);
    cout << "  -> PASS" << endl;
}

int main() {
    auto logger = init_logger("test-prosody", "warn");
    (void)logger;

    test_sentence_split();
    test_emotion_prosody();
    test_emotion_intensity();
    test_turn_head_tail();
    test_duration_estimate();
    test_prosody_to_engine_params();
    test_response_plan_basic();
    test_response_revision();
    test_cannot_discard_playing();
    test_no_stall_after_discard();
    test_token_stream_to_segments();
    test_emotion_reaches_segments();

    cout << "\nAll R6 prosody / response plan tests PASSED" << endl;
    return 0;
}
