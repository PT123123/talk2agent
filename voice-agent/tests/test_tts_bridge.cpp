// tests/test_tts_bridge.cpp
// §24 补完：TTS bridge 客户端（Qwen3-TTS / Chatterbox via 本地 Python 进程）
//
// 全部是**离线**测试：不起网络、不起 Python 进程、不加载任何模型。
// 因为 bridge 协议里最容易出错的地方恰恰是纯逻辑部分——
// WAV 字节布局、JSON 字段名、控制量钳制、降级判定。这些错了在真机上
// 表现为"合成不出来"，但没有任何栈信息可查，所以在单元测试里钉死。
#include "tts/tts_bridge.hpp"
#include "tts/qwen3_tts_adapter.hpp"
#include "tts/chatterbox_adapter.hpp"
#include "tts/prosody.hpp"
#include "util/log.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std;
using namespace voice_agent;

namespace {

// 造一个最小合法 WAV（44 字节标准头 + data）
std::string make_wav(const std::vector<int16_t>& samples, int sample_rate,
                     int channels = 1, int bits = 16) {
    std::string w;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) w.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
    };
    auto u16 = [&](uint16_t v) {
        for (int i = 0; i < 2; ++i) w.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
    };
    const uint32_t data_size = static_cast<uint32_t>(samples.size() * 2);
    const uint32_t byte_rate = static_cast<uint32_t>(sample_rate * channels * bits / 8);
    const uint16_t block_align = static_cast<uint16_t>(channels * bits / 8);

    w += "RIFF"; u32(36 + data_size); w += "WAVE";
    w += "fmt "; u32(16);
    u16(1); u16(static_cast<uint16_t>(channels));
    u32(static_cast<uint32_t>(sample_rate));
    u32(byte_rate); u16(block_align); u16(static_cast<uint16_t>(bits));
    w += "data"; u32(data_size);
    for (int16_t s : samples) u16(static_cast<uint16_t>(s));
    return w;
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

}  // namespace

// ============================================================
// 1. 引擎枚举往返（配置字符串 ↔ 枚举）
// ============================================================
static void test_engine_enum() {
    cout << "TEST engine enum roundtrip..." << endl;
    assert(tts_bridge_engine_from_string("qwen3tts") == TtsBridgeEngine::Qwen3Tts);
    assert(tts_bridge_engine_from_string("chatterbox") == TtsBridgeEngine::Chatterbox);
    // 未知引擎必须返回 Unknown（而不是猜一个）——TTS::initialize 靠这个
    // 决定"要不要走 bridge 分支"，猜错会把普通引擎拉去连 bridge。
    assert(tts_bridge_engine_from_string("kokoro") == TtsBridgeEngine::Unknown);
    assert(tts_bridge_engine_from_string("") == TtsBridgeEngine::Unknown);
    assert(tts_bridge_engine_from_string("simple") == TtsBridgeEngine::Unknown);
    // 大小写敏感：配置里写错大小写应该被当成"不是这个引擎"
    assert(tts_bridge_engine_from_string("Qwen3TTS") == TtsBridgeEngine::Unknown);

    assert(string(tts_bridge_engine_name(TtsBridgeEngine::Qwen3Tts)) == "qwen3tts");
    assert(string(tts_bridge_engine_name(TtsBridgeEngine::Chatterbox)) == "chatterbox");
    assert(string(tts_bridge_engine_name(TtsBridgeEngine::Unknown)) == "unknown");
    cout << "  -> PASS" << endl;
}

// ============================================================
// 2. WAV 解析
// ============================================================
static void test_wav_parse() {
    cout << "TEST wav parse..." << endl;

    vector<int16_t> pcm = {0, 1000, -1000, 32767, -32768, 42};
    std::string wav = make_wav(pcm, 24000);

    vector<int16_t> out;
    int sr = 0, ch = 0;
    assert(parse_wav_pcm16(wav, out, sr, ch));
    assert(sr == 24000);
    assert(ch == 1);
    assert(out.size() == pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        // 端序必须是 little-endian：值 -32768 写成 0x0080，
        // 大端解析会得到 128 —— 这类错误在真机上表现为"全是噪音"。
        assert(out[i] == pcm[i]);
    }
    assert(out[3] == 32767);
    assert(out[4] == -32768);

    // 非法输入必须安全返回 false（不能崩，也不能给半截数据）
    vector<int16_t> junk;
    assert(!parse_wav_pcm16("", junk, sr, ch));
    assert(!parse_wav_pcm16("RIFF", junk, sr, ch));
    assert(!parse_wav_pcm16(std::string(200, 'x'), junk, sr, ch));
    // 头合法但没有 data chunk
    {
        std::string no_data = make_wav(pcm, 24000);
        no_data.replace(no_data.find("data"), 4, "junk");
        assert(!parse_wav_pcm16(no_data, junk, sr, ch));
    }
    // 8-bit WAV（bridge 不会产出，但解析器不该把它当合法输入放行）
    assert(!parse_wav_pcm16(make_wav(pcm, 24000, 1, 8), junk, sr, ch));
    cout << "  -> PASS" << endl;
}

// ============================================================
// 3. 多声道 + 带 pad 的 chunk 遍历
// ============================================================
static void test_wav_multichannel_and_padding() {
    cout << "TEST wav multichannel + odd chunk padding..." << endl;

    // 立体声：L R L R → 取首声道得L L
    vector<int16_t> st = {100, -100, 200, -200, 300, -300};
    std::string wav = make_wav(st, 24000, 2);
    vector<int16_t> out;
    int sr = 0, ch = 0;
    assert(parse_wav_pcm16(wav, out, sr, ch));
    assert(ch == 2);
    assert(out.size() == 3);
    assert(out[0] == 100 && out[1] == 200 && out[2] == 300);

    // fmt 后面插一个奇数长度的 chunk，正确解析器要靠 pad 对齐继续走，
    // 找不到 data 就该返回 false 而不是读出垃圾。
    {
        std::string w;
        auto u32 = [&](uint32_t v) {
            for (int i = 0; i < 4; ++i) w.push_back(static_cast<char>((v >> (i*8)) & 0xFF));
        };
        auto u16 = [&](uint16_t v) {
            for (int i = 0; i < 2; ++i) w.push_back(static_cast<char>((v >> (i*8)) & 0xFF));
        };
        w += "RIFF"; u32(0); w += "WAVE";
        w += "fmt "; u32(16);
        u16(1); u16(1); u32(24000); u32(48000); u16(2); u16(16);
        // 奇数长度 chunk（3 字节）+ 1 字节 pad
        w += "JUNK"; u32(3); w.push_back('a'); w.push_back('b'); w.push_back('c');
        w.push_back('\0');
        w += "data"; u32(4);
        u16(11); u16(22);
        vector<int16_t> out2;
        int sr2 = 0, ch2 = 0;
        // 没有 pad 对齐就会读错 —— 断言我们确实处理对了
        assert(parse_wav_pcm16(w, out2, sr2, ch2));
        assert(out2.size() == 2 && out2[0] == 11 && out2[1] == 22);
    }
    cout << "  -> PASS" << endl;
}

// ============================================================
// 4. 重采样
// ============================================================
static void test_resample() {
    cout << "TEST resample..." << endl;

    // 同采样率必须原样返回（不引入任何数值变化）
    vector<int16_t> a = {0, 100, 200, 300};
    auto same = resample_linear(a, 24000, 24000);
    assert(same == a);

    // 8k → 24k（3倍）：时长守恒 + 单调性保持
    vector<int16_t> b;
    for (int i = 0; i < 800; ++i) b.push_back(static_cast<int16_t>(i * 100));
    auto up = resample_linear(b, 8000, 24000);
    assert(up.size() >= 2390 && up.size() <= 2410);   // 800*3 = 2400
    // 升采样后不应出现比输入最大值更大的野值（过冲 = 削波失真）
    for (int16_t v : up) {
        assert(v >= 0 && v <= 800 * 100);
    }

    // 24k → 16k（降采样）：不能崩溃，长度按比例
    auto down = resample_linear(b, 24000, 16000);
    assert(down.size() == static_cast<size_t>(std::llround(b.size() * 16000.0 / 24000.0)));

    // 边界：空输入、非法采样率都必须安全返回
    assert(resample_linear({}, 8000, 24000).empty());
    assert(resample_linear(b, 0, 24000) == b);      // 非法源采样率 → 原样
    assert(resample_linear(b, 24000, 0) == b);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 5. JSON 请求体组装 —— 字段名必须与 Python 端严格一致
// ============================================================
static void test_build_body() {
    cout << "TEST build synthesize body..." << endl;

    TtsBridgeControls c;
    c.instruction = "温柔亲切、急切";
    c.voice = "Cherry";
    c.speed = 1.15f;
    c.exaggeration = 0.72f;
    c.cfg_weight = 0.35f;
    c.lang = "zh";

    std::string body = build_synthesize_body("你好，世界", c, 24000);
    json j = json::parse(body);

    // 这七个字段名与 scripts/tts_bridge_server.py 的 req.get() 一一对应。
    // 任何一个拼错，Python 端会静默用默认值（.get 返回 None），
    // 表现是"风格控制不起作用"而不是报错 —— 最难查的一类 bug。
    assert(j["text"] == "你好，世界");
    assert(j["instruction"] == "温柔亲切、急切");
    assert(j["voice"] == "Cherry");
    assert(near(j["speed"].get<float>(), 1.15f));
    assert(near(j["exaggeration"].get<float>(), 0.72f));
    assert(near(j["cfg_weight"].get<float>(), 0.35f));
    assert(j["lang"] == "zh");
    assert(j["sample_rate"] == 24000);

    // UTF-8 中文必须原样编码（不能被转成 \uXXXX 之外的东西，
    // nlohmann 默认不转义非ASCII，这里断言的是"内容正确"）
    assert(body.find("温柔亲切") != std::string::npos);

    // 空 control 也要发出完整字段集 —— Python 端 .get(k, default) 之所以
    // 能工作，前提是字段存在或缺失时行为确定，不能有时发有时不发。
    TtsBridgeControls empty;
    json j2 = json::parse(build_synthesize_body("hi", empty, 16000));
    assert(j2["instruction"] == "");
    assert(j2["voice"] == "");
    assert(j2["sample_rate"] == 16000);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 6. /health 解析 —— 三态 + 保守策略
// ============================================================
static void test_health_parse() {
    cout << "TEST health parse (three-state)..." << endl;

    std::string detail;

    // ready
    detail.clear();
    assert(parse_health_state(
        R"({"ok":true,"backend_ready":true,"backend_state":"ready","detail":"loaded"})",
        &detail) == TtsBridgeHealth::Ready);
    assert(detail == "loaded");

    // loading：与 unavailable 严格区分。这是启动竞态的关键 ——
    // bridge 端口已开但权重还在加载，此时回退等于永久放弃好引擎。
    detail.clear();
    assert(parse_health_state(
        R"({"ok":true,"backend_ready":false,"backend_state":"loading","detail":"loading weights"})",
        &detail) == TtsBridgeHealth::Loading);
    assert(detail == "loading weights");

    // error → 不可用，原因要能带出来
    detail.clear();
    assert(parse_health_state(
        R"({"ok":true,"backend_ready":false,"backend_state":"error","detail":"qwen-tts 未安装"})",
        &detail) == TtsBridgeHealth::Unavailable);
    assert(detail.find("未安装") != std::string::npos);

    // 老版本 bridge（只有 backend_ready 无 backend_state）：必须仍能工作
    assert(parse_health_state(R"({"ok":true,"backend_ready":true})", &detail)
           == TtsBridgeHealth::Ready);
    assert(parse_health_state(R"({"ok":true,"backend_ready":false})", &detail)
           == TtsBridgeHealth::Unavailable);

    // ok=false → 不可用
    assert(parse_health_state(R"({"ok":false,"error":"boom"})", &detail)
           == TtsBridgeHealth::Unavailable);

    // 非 JSON / 空 body / HTML 错误页 → 不可用，且不能抛异常
    assert(parse_health_state("", &detail) == TtsBridgeHealth::Unavailable);
    assert(parse_health_state("not json at all", &detail)
           == TtsBridgeHealth::Unavailable);
    assert(parse_health_state("<html>502 Bad Gateway</html>", &detail)
           == TtsBridgeHealth::Unavailable);

    // 类型错误（backend_ready 是字符串 / backend_state 是数字）不能被当成 true
    assert(parse_health_state(R"({"ok":true,"backend_ready":"yes"})", &detail)
           == TtsBridgeHealth::Unavailable);
    assert(parse_health_state(R"({"ok":true,"backend_state":123})", &detail)
           == TtsBridgeHealth::Unavailable);

    // 未知状态字符串 → 不可用（不猜）
    assert(parse_health_state(R"({"ok":true,"backend_state":"weird"})", &detail)
           == TtsBridgeHealth::Unavailable);

    // 状态名可读（供日志）
    assert(string(tts_bridge_health_name(TtsBridgeHealth::Ready)) == "ready");
    assert(string(tts_bridge_health_name(TtsBridgeHealth::Loading)) == "loading");
    assert(string(tts_bridge_health_name(TtsBridgeHealth::Unavailable))
           == "unavailable");
    cout << "  -> PASS" << endl;
}

// ============================================================
// 7. 端到端：适配器算出的 controls 必须真的进到 bridge 请求体里
//    （这是本次改动的核心承诺：不再"算好放着"）
// ============================================================
static void test_adapter_controls_reach_request() {
    cout << "TEST adapter controls reach bridge request..." << endl;

    // --- Qwen3-TTS：instruction 必须出现在请求体里 ---
    {
        Qwen3TtsAdapter a(nullptr);
        Prosody warm;
        warm.warmth = 0.95f;
        warm.certainty = 0.1f;      // 低确定 → 带迟疑
        warm.urgency  = 0.95f;      // 高紧迫 → 急切
        a.apply_prosody(warm);

        const auto& c = a.last_controls();
        TtsBridgeControls bc;
        bc.instruction = c.instruction;
        bc.speed = c.speed;
        bc.lang = "zh";
        json j = json::parse(build_synthesize_body("测试", bc, 24000));

        // 断言"映射结果 → JSON → Python 端能读到"整条链路上值没丢
        assert(j["instruction"] == c.instruction);
        assert(j["instruction"].get<std::string>().find("温柔") != std::string::npos);
        assert(j["instruction"].get<std::string>().find("急切") != std::string::npos);
        assert(near(j["speed"].get<float>(), c.speed));
    }

    // --- Chatterbox：exaggeration / cfg_weight 必须出现在请求体里 ---
    {
        ChatterboxAdapter a(nullptr);
        Prosody hot;
        hot.energy = 0.95f;
        hot.warmth = 0.95f;
        hot.certainty = 0.95f;
        hot.urgency = 0.95f;
        hot.pace = 0.9f;
        a.apply_prosody(hot);

        const auto& c = a.last_controls();
        TtsBridgeControls bc;
        bc.exaggeration = c.exaggeration;
        bc.cfg_weight = c.cfg_weight;
        bc.speed = c.speed;
        json j = json::parse(build_synthesize_body("测试", bc, 24000));

        assert(near(j["exaggeration"].get<float>(), c.exaggeration));
        assert(near(j["cfg_weight"].get<float>(), c.cfg_weight));
        // 高能量高温度 → exaggeration 必须真的被推高（否则等于没接）
        assert(c.exaggeration > 0.8f);
        // 高紧迫 → cfg_weight 被压低（更松弛）
        assert(c.cfg_weight < 0.6f);
        // 两个引擎的官方区间
        assert(c.exaggeration >= 0.0f && c.exaggeration <= 1.0f);
        assert(c.cfg_weight >= 0.1f && c.cfg_weight <= 1.0f);
    }

    // --- 韵律变化 → controls 单调（防止"接上了但值是常量"的退化）---
    {
        Qwen3TtsAdapter q(nullptr);
        q.apply_prosody(Prosody{.warmth = 0.05f});
        const string cool = q.last_controls().instruction;
        q.apply_prosody(Prosody{.warmth = 0.95f});
        const string warm = q.last_controls().instruction;
        assert(cool != warm);

        ChatterboxAdapter c(nullptr);
        c.apply_prosody(Prosody{.energy = 0.1f, .warmth = 0.1f});
        float lo = c.last_controls().exaggeration;
        c.apply_prosody(Prosody{.energy = 0.95f, .warmth = 0.95f});
        float hi = c.last_controls().exaggeration;
        assert(hi > lo);
    }
    cout << "  -> PASS" << endl;
}

// ============================================================
// 8. 适配器在无 TTS / 无 bridge 时仍必须安全
// ============================================================
static void test_adapter_safety() {
    cout << "TEST adapter safety (null tts)..." << endl;

    Qwen3TtsAdapter q(nullptr);
    bool called = false, last = false;
    q.synthesize(SpeechSegment{.text = "你好"},
                 [&](const int16_t*, size_t, bool is_last) {
                     called = true; last = is_last;
                 });
    assert(called && last);

    called = last = false;
    ChatterboxAdapter cb(nullptr);
    cb.synthesize(SpeechSegment{.text = ""},
                  [&](const int16_t*, size_t, bool is_last) {
                      called = true; last = is_last;
                  });
    assert(called && last);   // 空文本也必须回调 is_last，否则播放游标卡死

    // apply_prosody 在无 TTS 时不能崩，且仍要算得出 controls
    q.apply_prosody(Prosody{.warmth = 0.95f});
    assert(!q.last_controls().instruction.empty());
    cout << "  -> PASS" << endl;
}

int main() {
    cout << "==== test_tts_bridge ====" << endl;
    test_engine_enum();
    test_wav_parse();
    test_wav_multichannel_and_padding();
    test_resample();
    test_build_body();
    test_health_parse();
    test_adapter_controls_reach_request();
    test_adapter_safety();
    cout << "ALL PASS" << endl;
    return 0;
}