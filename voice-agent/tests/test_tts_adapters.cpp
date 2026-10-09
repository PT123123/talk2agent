// tests/test_tts_adapters.cpp
// §24: Qwen3-TTS / Chatterbox TTS 适配器（韵律 → 引擎控制映射）
//
// 这些映射是纯逻辑（不依赖任何模型/音频），可以离线精确断言。
// 重点验证：每个引擎暴露的控制旋钮能正确反映统一韵律语义，
// 以及适配器在"无底层引擎"时的安全回退。
#include "tts/qwen3_tts_adapter.hpp"
#include "tts/chatterbox_adapter.hpp"
#include "tts/prosody.hpp"
#include "util/log.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

using namespace std;
using namespace voice_agent;

namespace {
bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }
}  // namespace

// ============================================================
// 1. Qwen3-TTS：韵律 → 自然语言 instruction
// ============================================================
static void test_qwen3_instruction() {
    cout << "TEST qwen3 instruction mapping..." << endl;

    // 温暖 → 温柔亲切
    Prosody warm; warm.warmth = 0.95f;
    auto c1 = map_prosody_for_qwen3(warm);
    assert(contains(c1.instruction, "温柔"));
    assert(contains(c1.instruction, "亲切"));

    // 冷静 → 冷静克制
    Prosody cool; cool.warmth = 0.1f;
    auto c2 = map_prosody_for_qwen3(cool);
    assert(contains(c2.instruction, "冷静"));

    // 高能量 + 高紧迫 → 充满活力 / 急切
    Prosody hot; hot.energy = 0.95f; hot.urgency = 0.95f;
    auto c3 = map_prosody_for_qwen3(hot);
    assert(contains(c3.instruction, "活力"));
    assert(contains(c3.instruction, "急切"));

    // 低确定 → 带着些许迟疑
    Prosody unsure; unsure.certainty = 0.1f;
    auto c4 = map_prosody_for_qwen3(unsure);
    assert(contains(c4.instruction, "迟疑"));

    // 中性基线 → 自然清晰
    Prosody neutral;  // 全默认
    auto c5 = map_prosody_for_qwen3(neutral);
    assert(c5.instruction == "自然清晰");

    // speed 随语速基线单调
    Prosody fast; fast.pace = 0.95f;
    Prosody slow; slow.pace = 0.05f;
    assert(map_prosody_for_qwen3(fast).speed > map_prosody_for_qwen3(slow).speed);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 2. Chatterbox：韵律 → exaggeration / cfg_weight
// ============================================================
static void test_chatterbox_controls() {
    cout << "TEST chatterbox controls mapping..." << endl;

    // 高能量 + 高温度 → 更高 exaggeration
    ChatterboxControls hi = map_prosody_for_chatterbox(
        Prosody{.energy = 0.95f, .warmth = 0.95f});
    ChatterboxControls lo = map_prosody_for_chatterbox(
        Prosody{.energy = 0.1f, .warmth = 0.1f});
    assert(hi.exaggeration > lo.exaggeration);
    assert(hi.exaggeration >= 0.0f && hi.exaggeration <= 1.0f);

    // 确定高 → cfg_weight 更高（更克制清晰）
    ChatterboxControls sure = map_prosody_for_chatterbox(
        Prosody{.certainty = 0.95f, .urgency = 0.3f});
    ChatterboxControls unsure = map_prosody_for_chatterbox(
        Prosody{.certainty = 0.05f, .urgency = 0.3f});
    assert(sure.cfg_weight > unsure.cfg_weight);
    assert(sure.cfg_weight >= 0.1f && sure.cfg_weight <= 1.0f);

    // 紧迫高 → cfg_weight 更低（更松弛、想快）
    ChatterboxControls urgent = map_prosody_for_chatterbox(
        Prosody{.certainty = 0.5f, .urgency = 0.95f});
    ChatterboxControls calm = map_prosody_for_chatterbox(
        Prosody{.certainty = 0.5f, .urgency = 0.05f});
    assert(urgent.cfg_weight < calm.cfg_weight);

    // speed 随语速基线单调
    ChatterboxControls fast = map_prosody_for_chatterbox(
        Prosody{.certainty = 0.6f, .urgency = 0.3f, .pace = 0.95f});
    ChatterboxControls slow = map_prosody_for_chatterbox(
        Prosody{.certainty = 0.6f, .urgency = 0.3f, .pace = 0.05f});
    assert(fast.speed > slow.speed);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 3. 适配器元数据 + 安全回退（无底层引擎）
// ============================================================
static void test_adapter_meta_and_fallback() {
    cout << "TEST adapter meta + fallback..." << endl;

    // 元数据
    Qwen3TtsAdapter q(nullptr);
    assert(q.name() == "qwen3tts");
    assert(q.supports_full_prosody() == true);

    ChatterboxAdapter cb(nullptr);
    assert(cb.name() == "chatterbox");
    assert(cb.supports_full_prosody() == true);

    // 无底层引擎时 synthesize 必须安全回调 is_last，绝不崩溃
    bool called = false;
    bool last = false;
    q.synthesize(SpeechSegment{.text = "你好"},
                 [&](const int16_t*, size_t, bool is_last) {
                     called = true; last = is_last;
                 });
    assert(called && last);

    called = false; last = false;
    cb.synthesize(SpeechSegment{.text = "你好"},
                  [&](const int16_t*, size_t, bool is_last) {
                      called = true; last = is_last;
                  });
    assert(called && last);

    // 空文本同样安全
    called = false; last = false;
    q.synthesize(SpeechSegment{.text = ""},
                 [&](const int16_t*, size_t, bool is_last) {
                     called = true; last = is_last;
                 });
    assert(called && last);

    // apply_prosody 会算出控制并保留（供后端消费）
    q.apply_prosody(Prosody{.warmth = 0.95f});
    assert(contains(q.last_controls().instruction, "温柔"));
    assert(near(q.last_controls().speed, ProsodyPlanner::rate_from_prosody(
                   Prosody{.warmth = 0.95f}), 0.001f));
    cb.apply_prosody(Prosody{.energy = 0.9f, .warmth = 0.9f});
    assert(cb.last_controls().exaggeration >= 0.0f);
    cout << "  -> PASS" << endl;
}

int main() {
    cout << "==== test_tts_adapters ====" << endl;
    test_qwen3_instruction();
    test_chatterbox_controls();
    test_adapter_meta_and_fallback();
    cout << "ALL PASS" << endl;
    return 0;
}
