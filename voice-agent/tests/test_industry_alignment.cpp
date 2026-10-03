// tests/test_industry_alignment.cpp
// R9: 对标业界方案的验证
//   - 打断上下文同步（对标 OpenAI conversation.item.truncate / Azure auto_truncate）
//   - 语义轮次判定（对标 OpenAI semantic_vad + eagerness）
#include "orchestrator/interruption_truncation.hpp"
#include "orchestrator/semantic_turn.hpp"
#include "orchestrator/audio_router.hpp"
#include "util/log.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace std;
using namespace voice_agent;

namespace {
const char* hint_name(CompletionHint h) {
    switch (h) {
        case CompletionHint::Incomplete: return "incomplete";
        case CompletionHint::LikelyDone: return "likely_done";
        case CompletionHint::Uncertain:  return "uncertain";
    }
    return "?";
}
}

// ============================================================
// 1. 打断截断：用户听到的必须被保留
// ============================================================
static void test_truncate_keeps_played() {
    cout << "TEST interruption truncation keeps played portion..." << endl;

    InterruptionTruncation t;
    const string full = "第一个方案是本地的。第二个方案是在线的。第三个是混合的。";

    // 用户只听到了前 40%
    auto r = t.truncate(full, 0.4);
    assert(r.truncated);
    assert(!r.played_text.empty());
    assert(!r.dropped_text.empty());
    assert(r.played_text.size() + r.dropped_text.size() >=
           full.size() - 10);   // 只允许标点级误差
    // 保留的部分是前缀
    assert(full.find(r.played_text.substr(0, 6)) == 0);
    cout << "  (kept=" << r.played_text.size() << " dropped="
         << r.dropped_text.size() << ") -> PASS" << endl;
}

// ============================================================
// 2. 几乎没播出时整段丢弃
// ============================================================
static void test_truncate_nothing_played() {
    cout << "TEST interruption with nothing played..." << endl;

    InterruptionTruncation t;
    const string full = "这是完整的回答内容。";

    auto r = t.truncate(full, 0.0);
    assert(r.truncated);
    assert(r.played_text.empty());
    assert(r.dropped_text == full);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 3. 全部播完时不算截断
// ============================================================
static void test_no_truncation_when_complete() {
    cout << "TEST no truncation when fully played..." << endl;

    InterruptionTruncation t;
    const string full = "完整播出的回答。";
    auto r = t.truncate(full, 1.0);
    assert(!r.truncated);
    assert(r.played_text == full);
    assert(r.dropped_text.empty());
    cout << "  -> PASS" << endl;
}

// ============================================================
// 4. 截断标记：让模型知道自己被打断了（对应 Azure appended_text_after_truncation）
// ============================================================
static void test_interrupt_marker_present() {
    cout << "TEST interrupt marker present..." << endl;

    InterruptionTruncation t;
    auto r = t.truncate("说了很长一段话但是被打断了。", 0.5);
    assert(r.played_text.find("被用户打断") != string::npos);

    // 给模型的说明也要有
    const string note = t.build_interrupt_note(r);
    assert(!note.empty());
    assert(note.find("被打断") != string::npos);
    assert(note.find("不要假设用户听到了") != string::npos);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 5. 段级截断（比字符比例更准）
// ============================================================
static void test_truncate_by_segments() {
    cout << "TEST segment-level truncation..." << endl;

    InterruptionTruncation t;
    vector<string> all = {"第一句。", "第二句。", "第三句。", "第四句。"};
    vector<string> played = {"第一句。", "第二句。"};

    auto r = t.truncate_by_segments(played, all);
    assert(r.truncated);
    assert(r.played_ratio == 0.5);
    assert(r.played_text.find("第一句") != string::npos);
    assert(r.played_text.find("第二句") != string::npos);
    assert(r.played_text.find("第三句") == string::npos);
    assert(r.dropped_text.find("第三句") != string::npos);
    assert(r.dropped_text.find("第四句") != string::npos);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 6. 语义完备性：连接词结尾 = 没说完
// ============================================================
static void test_semantic_incomplete() {
    cout << "TEST semantic completeness: connectors mean incomplete..." << endl;

    SemanticTurnDetector d;

    // 明显的连接词结尾 —— 纯时长阈值会在这里抢话
    assert(d.analyze("我想问一下那个") == CompletionHint::Incomplete);
    assert(d.analyze("因为这个方案") == CompletionHint::Incomplete);
    assert(d.analyze("首先是") == CompletionHint::Incomplete);
    assert(d.analyze("让我想一下...") == CompletionHint::Incomplete);

    // 逗号结尾 —— 铁定没说完
    assert(d.analyze("这个项目有三个方面，") == CompletionHint::Incomplete);

    // 无标点无连接词
    assert(d.analyze("嗯我今天") == CompletionHint::Incomplete);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 7. 语义完备性：完整句 = 说完了
// ============================================================
static void test_semantic_likely_done() {
    cout << "TEST semantic completeness: complete sentences..." << endl;

    SemanticTurnDetector d;

    assert(d.analyze("现在几点。") == CompletionHint::LikelyDone);
    assert(d.analyze("帮我查一下天气") == CompletionHint::LikelyDone);
    assert(d.analyze("今天的会议几点开始？") == CompletionHint::LikelyDone);
    assert(d.analyze("总结一下这次改动") == CompletionHint::LikelyDone);

    // 极短 utterance 视为说完（多半是附和）
    assert(d.analyze("嗯") == CompletionHint::LikelyDone);
    assert(d.analyze("好") == CompletionHint::LikelyDone);
    assert(d.analyze("OK") == CompletionHint::LikelyDone);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 8. 核心行为：半句+长停顿不该让出，纯时长会抢话
// ============================================================
static void test_veto_on_incomplete_with_long_pause() {
    cout << "TEST incomplete sentence survives long pause..." << endl;

    SemanticTurnDetector d(SemanticTurnDetector::Config{Eagerness::Low, 6});

    // "我想问一下...(停 900ms)...那个项目" ——
    // 900ms 远超 EOUDetector 的 force_ms(900)，纯时长阈值会在这里抢话。
    assert(!d.should_yield("我想问一下那个", 900));
    // 再等更久仍未过 max_wait（low = 8000ms）
    assert(!d.should_yield("我想问一下那个", 5000));
    // 到了 max_wait 必须兜底让出，否则用户彻底停顿时 Agent 永远沉默
    assert(d.should_yield("我想问一下那个", 8000));

    // 对比：完整句在 500ms（low 的 min_wait）就该让出
    assert(d.should_yield("现在几点。", 600));
    cout << "  -> PASS" << endl;
}

// ============================================================
// 9. Eagerness 越低越保守
// ============================================================
static void test_eagerness_ordering() {
    cout << "TEST eagerness ordering (low waits longest)..." << endl;

    SemanticTurnDetector low(SemanticTurnDetector::Config{Eagerness::Low, 6});
    SemanticTurnDetector high(SemanticTurnDetector::Config{Eagerness::High, 6});

    const string incomplete = "我想问一下那个";
    // 同样 2500ms：high 已经在 max_wait(2000) 之外，low 还没到
    assert(high.should_yield(incomplete, 2500));
    assert(!low.should_yield(incomplete, 2500));

    // OpenAI 官方的超时：low=8s medium=4s high=2s
    assert(eagerness_config(Eagerness::Low).max_wait_ms == 8000);
    assert(eagerness_config(Eagerness::Medium).max_wait_ms == 4000);
    assert(eagerness_config(Eagerness::High).max_wait_ms == 2000);
    cout << "  (low=8s medium=4s high=2s, matches OpenAI) -> PASS" << endl;
}

// ============================================================
// 10. AudioRouter 播放进度：打断时能算出"听到了多少"
// ============================================================
static void test_audio_router_progress() {
    cout << "TEST audio router playback progress..." << endl;

    AudioRouter r;
    assert(r.played_ratio() == 0.0);

    r.start_playback();
    const int16_t chunk[480] = {0};   // 200ms @ 24kHz
    r.push_tts_frames(chunk, 480);
    assert(r.total_frames_pushed() == 480);
    // 还没播放 —— pushed 但 played 为 0
    assert(r.total_frames_played() == 0);
    assert(r.played_ratio() == 0.0);

    int16_t out[240];
    r.get_playback_frames(out, 240);      // 播一半
    assert(r.total_frames_played() == 240);
    const double ratio = r.played_ratio();
    assert(ratio > 0.49 && ratio < 0.51); // ≈0.5

    // 关键：未播出的部分就是打断后要截断的
    assert(r.total_frames_pushed() > r.total_frames_played());

    r.reset_progress();
    assert(r.total_frames_pushed() == 0);
    cout << "  (pushed=480 played=240 ratio=" << (int)(ratio * 100)
         << "%) -> PASS" << endl;
}

int main() {
    auto logger = init_logger("test-industry-alignment", "warn");
    (void)logger;

    test_truncate_keeps_played();
    test_truncate_nothing_played();
    test_no_truncation_when_complete();
    test_interrupt_marker_present();
    test_truncate_by_segments();
    test_semantic_incomplete();
    test_semantic_likely_done();
    test_veto_on_incomplete_with_long_pause();
    test_eagerness_ordering();
    test_audio_router_progress();

    cout << "\nAll R9 industry-alignment tests PASSED" << endl;
    return 0;
}
