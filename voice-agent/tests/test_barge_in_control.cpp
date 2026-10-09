// tests/test_barge_in_control.cpp
// 打断控制验证：AudioRouter 的淡出 / duck / 播放进度，以及 barge_in 配置解析。
//
// 这一层是纯逻辑，不需要麦克风/模型/声卡 —— 打断时序是全项目最容易改坏
// 又最难靠肉眼发现的部分（淡出没生效、duck 没复原、进度算错导致上下文
// 截断丢内容），所以必须能离线精确断言。
#include "orchestrator/audio_router.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace std;
using namespace voice_agent;

namespace {

int g_failed = 0;

void check(bool cond, const string& what) {
    if (cond) {
        cout << "  [PASS] " << what << endl;
    } else {
        cout << "  [FAIL] " << what << endl;
        ++g_failed;
    }
}

void check_near(double a, double b, double tol, const string& what) {
    check(std::fabs(a - b) <= tol, what);
}

constexpr int kRate = 24000;

// 生成一段恒定幅度方波，便于观察增益变化
vector<int16_t> make_tone(size_t frames, int16_t amp = 8000) {
    return vector<int16_t>(frames, amp);
}

// 把缓冲里所有帧拉出来（模拟播放回调连续拉取），返回本次实际拉出的帧数。
//
// 注意 get_playback_frames() 只返回"还有没有"，不返回"这次写了多少帧"，
// 最后一次拉取通常是部分填充。所以帧数必须靠 total_frames_played() 的
// 增量来数 —— 直接数累计的 buf 大小会把最后一块算成满块，是错的。
int64_t drain(AudioRouter& r, std::vector<int16_t>& out, size_t chunk = 480) {
    std::vector<int16_t> buf(chunk);
    const int64_t before = r.total_frames_played();
    while (true) {
        const int64_t chunk_before = r.total_frames_played();
        bool more = r.get_playback_frames(buf.data(), chunk);
        const size_t wrote =
            static_cast<size_t>(r.total_frames_played() - chunk_before);
        out.insert(out.end(), buf.begin(), buf.begin() + wrote);
        if (!more) break;
    }
    return r.total_frames_played() - before;
}

}  // namespace

// ---- 1. 硬切：stop_playback 立刻丢缓冲，播放游标归零 ----
static void test_hard_stop_clears() {
    cout << "TEST hard stop clears buffer..." << endl;
    AudioRouter r;
    r.set_output_rate(kRate);
    r.start_playback();
    r.push_tts_frames(make_tone(4800).data(), 4800);
    assert(r.played_ratio() == 0.0);
    r.stop_playback();

    check(!r.is_playing(), "stop_playback -> not playing");
    int16_t buf[480];
    check(!r.get_playback_frames(buf, 480), "stop_playback -> no more frames");
    check(r.played_ratio() == 0.0, "ratio reset to 0 after new turn (not after stop)");
}

// ---- 2. 播放进度：已推送 ≠ 已播放 ----
static void test_played_ratio() {
    cout << "TEST played vs pushed ratio..." << endl;
    AudioRouter r;
    r.set_output_rate(kRate);
    r.start_playback();
    r.push_tts_frames(make_tone(10000).data(), 10000);

    int16_t buf[1000];
    // 只拉 4000 帧（=40%），模拟"模型已生成 10000 但只播了 4000"
    r.get_playback_frames(buf, 1000);
    r.get_playback_frames(buf, 1000);
    r.get_playback_frames(buf, 1000);
    r.get_playback_frames(buf, 1000);

    check(r.total_frames_pushed() == 10000, "pushed == 10000");
    check(r.total_frames_played() == 4000, "played == 4000");
    check_near(r.played_ratio(), 0.4, 1e-6, "ratio == 0.4 (what user actually heard)");
}

// ---- 3. 淡出：这是本次修复的核心 ----
// 修复前 fading_out_ / fade_out_start_frame_ / fade_out_frames_ 从未被赋值，
// 打断走的是 tts_buffer_.clear() 硬切。这里验证淡出真的发生。
static void test_fade_out_actually_fades() {
    cout << "TEST fade-out really fades (was dead code)..." << endl;
    AudioRouter r;
    r.set_output_rate(kRate);
    r.set_fade_out_ms(40);              // 40ms @24kHz = 960 帧
    r.start_playback();
    // 缓冲 4000 帧，其中前1000 帧已被"播放"掉
    r.push_tts_frames(make_tone(4000).data(), 4000);
    {
        int16_t buf[1000];
        r.get_playback_frames(buf, 1000);   // 消费掉1000 帧
    }
    check_near(r.played_ratio(), 0.25, 1e-6, "pre-fade ratio 0.25");

    r.request_fade_out_stop();

    // 淡出期间仍在"播放"（is_playing 为真），播放回调才能把尾巴拉出来
    check(r.is_playing(), "still playing during fade-out");
    check(r.played_ratio() > 0.24 && r.played_ratio() <= 0.26,
          "ratio ~unchanged at fade start (tail not yet played)");

    std::vector<int16_t> out;
    const int64_t faded = drain(r, out, 480);

    check_near(static_cast<double>(faded), 960.0, 1.0,
               "faded exactly 960 frames (40ms @24kHz)");
    check(!r.is_playing() || r.played_ratio() > 0.25,
          "fade completed");

    // 关键：第一帧接近原幅度，最后一帧衰减到 ~0（无爆音）
    const int16_t first = out.front();
    const int16_t last = out.back();
    cout << "    first=" << first << " last=" << last << endl;
    check(std::abs(first) > 7000, "fade starts near full amplitude");
    check(std::abs(last) < 400, "fade ends near zero (no click at cut)");

    // 单调递减：不允许中途反弹
    bool monotonic = true;
    int16_t prev = out.front();
    for (size_t i = 1; i < out.size(); ++i) {
        if (std::abs(out[i]) > std::abs(prev) + 200) { monotonic = false; break; }
        prev = out[i];
    }
    check(monotonic, "gain decays monotonically");
}

// ---- 4. 淡出钳位：缓冲比淡出窗口还短时，只淡出剩余部分 ----
static void test_fade_out_clamped_to_remaining() {
    cout << "TEST fade-out clamped to remaining buffer..." << endl;
    AudioRouter r;
    r.set_output_rate(kRate);
    r.set_fade_out_ms(40);              // 960 帧
    r.start_playback();
    r.push_tts_frames(make_tone(100).data(), 100);   // 只剩 100 帧

    r.request_fade_out_stop();
    std::vector<int16_t> out;
    const int64_t faded = drain(r, out, 48);
    check_near(static_cast<double>(faded), 100.0, 1.0,
               "faded only the 100 available frames, not 960");
    check(std::abs(out.back()) < 400, "still ends near zero");
}

// ---- 5. 淡出时新帧被丢弃（否则边淡边补 = Agent 像在继续说）----
static void test_no_push_during_fade() {
    cout << "TEST new TTS frames dropped during fade-out..." << endl;
    AudioRouter r;
    r.set_output_rate(kRate);
    r.set_fade_out_ms(40);
    r.start_playback();
    r.push_tts_frames(make_tone(4000).data(), 4000);

    r.request_fade_out_stop();
    // 模拟"tts_->stop() 没来得及生效，还推来一块"
    const auto extra = make_tone(4800);
    r.push_tts_frames(extra.data(), extra.size());

    std::vector<int16_t> out;
    const int64_t faded = drain(r, out, 480);
    check_near(static_cast<double>(faded), 960.0, 1.0,
               "extra 4800 frames discarded, fade length unchanged");
}

// ---- 6. duck：门槛等待期间压低音量，但不停止播放 ----
static void test_duck_lowers_gain_without_stopping() {
    cout << "TEST duck lowers gain without stopping playback..." << endl;
    AudioRouter r;
    r.set_output_rate(kRate);
    r.start_playback();
    const auto tone = make_tone(4000);
    r.push_tts_frames(tone.data(), tone.size());

    r.set_output_gain(0.35f);
    check(r.is_playing(), "duck does not stop playback");

    std::vector<int16_t> out;
    const int64_t pulled = drain(r, out, 400);
    check_near(static_cast<double>(pulled), 4000.0, 1.0,
               "all frames still delivered while ducked");
    check(std::abs(out[100]) < 5000 && std::abs(out[100]) > 2000,
          "amplitude scaled to ~35%");

    r.set_output_gain(1.0f);
    check_near(r.output_gain(), 1.0, 1e-6, "gain restored to 1.0");
}

// ---- 7. duck 增益被钳在 [0,1] ----
static void test_gain_clamped() {
    cout << "TEST output gain clamped to [0,1]..." << endl;
    AudioRouter r;
    r.set_output_gain(-1.0f);
    check_near(r.output_gain(), 0.0, 1e-6, "negative gain -> 0");
    r.set_output_gain(5.0f);
    check_near(r.output_gain(), 1.0, 1e-6, "gain >1 -> 1");
}

// ---- 8. 新一轮播放重置 duck（上一轮的 duck 不能漏到下一轮）----
static void test_start_playback_resets_gain() {
    cout << "TEST start_playback resets duck..." << endl;
    AudioRouter r;
    r.set_output_rate(kRate);
    r.start_playback();
    r.set_output_gain(0.35f);
    r.start_playback();
    check_near(r.output_gain(), 1.0, 1e-6, "gain back to 1.0 on new turn");
}

// ---- 9. 未播放时请求淡出退化为硬切，不崩 ----
static void test_fade_out_without_playback() {
    cout << "TEST fade-out degenerates safely..." << endl;
    AudioRouter r;
    r.set_output_rate(kRate);
    // 从未 start_playback
    r.request_fade_out_stop();
    check(!r.is_playing(), "no crash when not playing");

    r.start_playback();
    // 在播但缓冲为空
    r.request_fade_out_stop();
    check(!r.is_playing(), "empty buffer -> hard stop");

    // 采样率兜底：没set_output_rate 时按 24000 处理，不能除零/溢出
    AudioRouter r2;
    r2.start_playback();
    r2.push_tts_frames(make_tone(2000).data(), 2000);
    r2.request_fade_out_stop();
    std::vector<int16_t> out;
    const int64_t faded = drain(r2, out, 480);
    check_near(static_cast<double>(faded), 960.0, 1.0,
               "default 24kHz assumption gives 960 frames");
}

// ---- 10. barge_in 配置解析 ----
static void test_barge_in_config_parsing() {
    cout << "TEST barge_in / input_mode config parsing..." << endl;
    const char* path = "test_barge_in_tmp.yaml";
    {
        FILE* f = fopen(path, "w");
        assert(f);
        fprintf(f,
            "audio:\n"
            "  sample_rate: 48000\n"
            "barge_in:\n"
            "  min_ms: 320\n"
            "  backchannel_max_ms: 700\n"
            "  fade_out_ms: 55\n"
            "  duck_volume: 0.5\n"
            "  require_threshold: false\n"
            "input_mode: ptt\n"
            "enable_interrupt_truncation: false\n"
            "eagerness: high\n");
        fclose(f);
    }
    AppConfig cfg = load_config(path);

    check(cfg.barge_in_min_ms == 320, "barge_in.min_ms parsed");
    check(cfg.backchannel_max_ms == 700, "backchannel_max_ms parsed");
    check(cfg.fade_out_ms == 55, "fade_out_ms parsed");
    check_near(cfg.interrupt_duck_volume, 0.5, 1e-6, "duck_volume parsed");
    check(cfg.barge_in_require_threshold == false, "require_threshold parsed");
    check(cfg.input_mode == "ptt", "input_mode=ptt parsed");
    check(cfg.enable_interrupt_truncation == false,
          "enable_interrupt_truncation parsed");
    check(cfg.eagerness == "high", "eagerness parsed");

    // 缺省值：没写 duck_volume / require_threshold 时必须走默认而不是崩
    {
        FILE* f = fopen("test_barge_in_min.yaml", "w");
        assert(f);
        fprintf(f, "barge_in:\n  min_ms: 100\n");
        fclose(f);
    }
    AppConfig d = load_config("test_barge_in_min.yaml");
    check_near(d.interrupt_duck_volume, 0.35, 1e-6, "duck_volume default 0.35");
    check(d.barge_in_require_threshold == true, "require_threshold default true");
    check(d.input_mode == "ptt", "input_mode default ptt");
    remove("test_barge_in_min.yaml");
    remove(path);
}

// ---- 11. 未知 input_mode 归一到 ptt（默认路径）----
static void test_input_mode_fallback() {
    cout << "TEST unknown input_mode falls back to ptt..." << endl;
    const char* path = "test_input_mode.yaml";
    {
        FILE* f = fopen(path, "w");
        assert(f);
        fprintf(f, "input_mode: garbage\n");
        fclose(f);
    }
    AppConfig cfg = load_config(path);
    check(cfg.input_mode == "ptt", "garbage -> ptt");

    // 显式写 vad 仍然要认，不能被默认吃掉
    {
        FILE* f = fopen(path, "w");
        assert(f);
        fprintf(f, "input_mode: vad\n");
        fclose(f);
    }
    AppConfig v = load_config(path);
    check(v.input_mode == "vad", "explicit vad still respected");
    remove(path);
}

// ---- 12. yaml 分组内回写（GUI 改设置要写回 agent.yaml）----
// 短 key（min_ms）如果全局搜，会命中 fade_out_ms 之类的同尾字段，
// 所以必须限定分组。这条路径错了会静默改错配置项，最难发现。
static void test_yaml_group_edit() {
    cout << "TEST yaml_edit::set_in_group scoping..." << endl;
    std::string text =
        "# top comment\n"
        "audio:\n"
        "  sample_rate: 48000\n"
        "barge_in:\n"
        "  min_ms: 160\n"
        "  fade_out_ms: 40\n"
        "eou:\n"
        "  fast_ms: 350\n"
        "input_mode: vad\n";

    //改 barge_in 内的 min_ms —— 不能误伤 fade_out_ms / eou.fast_ms
    bool ch = yaml_edit::set_in_group(text, "barge_in", "min_ms", "250");
    check(ch, "min_ms changed");
    check(text.find("  min_ms: 250\n") != std::string::npos, "min_ms updated in place");
    check(text.find("  fade_out_ms: 40\n") != std::string::npos,
          "fade_out_ms NOT collaterally modified");
    check(text.find("  fast_ms: 350\n") != std::string::npos,
          "eou.fast_ms NOT touched by barge_in edit");

    // 改 fade_out_ms（名字里含 min_ms 的那个，考验定位精度）
    yaml_edit::set_in_group(text, "barge_in", "fade_out_ms", "60");
    check(text.find("  fade_out_ms: 60\n") != std::string::npos, "fade_out_ms updated");
    check(text.find("  min_ms: 250\n") != std::string::npos, "min_ms still 250");

    // 同值写入 -> 无改动（避免无谓写盘）
    const std::string before = text;
    check(!yaml_edit::set_in_group(text, "barge_in", "min_ms", "250"),
          "same value -> reports no change");

    // 分组不存在 -> 不动文本、不崩
    const std::string t2 = text;
    check(!yaml_edit::set_in_group(text, "nosuch", "x", "1"),
          "missing group -> false");
    check(text == t2, "missing group -> text untouched");

    // 组内缺该 key -> 插到分组末尾，且缩进两空格
    check(yaml_edit::set_in_group(text, "barge_in", "duck_volume", "0.50"),
          "missing key in group -> inserted");
    check(text.find("  duck_volume: 0.50\n") != std::string::npos,
          "inserted with 2-space indent");
    // 插入位置必须在 eou: 之前
    const size_t ins = text.find("duck_volume");
    const size_t eou = text.find("eou:");
    check(ins != std::string::npos && ins < eou, "inserted inside barge_in, before eou:");

    (void)before;
}

// ---- 13. 端到端往返：改完再解析，值必须真的变了 ----
static void test_yaml_round_trip() {
    cout << "TEST barge_in write->parse round trip..." << endl;
    std::string text =
        "barge_in:\n"
        "  min_ms: 160\n"
        "  fade_out_ms: 40\n"
        "  duck_volume: 0.35\n"
        "  require_threshold: true\n"
        "input_mode: vad\n";

    // 模拟用户在 GUI 上把门槛调到 250ms、音量压到 20%
    yaml_edit::set_in_group(text, "barge_in", "min_ms", "250");
    yaml_edit::set_in_group(text, "barge_in", "duck_volume", "0.20");
    yaml_edit::set_in_group(text, "barge_in", "require_threshold", "false");
    // input_mode 是顶层 key，走 set_scalar 那条路径（这里只验证解析侧）
    const size_t im = text.find("input_mode: vad");
    text.replace(im, std::strlen("input_mode: vad"), "input_mode: ptt");

    const char* path = "test_barge_in_rt.yaml";
    FILE* f = fopen(path, "w");
    assert(f);
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);

    AppConfig c = load_config(path);
    check(c.barge_in_min_ms == 250, "round trip min_ms == 250");
    check_near(c.interrupt_duck_volume, 0.20, 1e-6, "round trip duck_volume == 0.20");
    check(c.barge_in_require_threshold == false,
          "round trip require_threshold == false");
    check(c.fade_out_ms == 40, "round trip fade_out_ms preserved");
    check(c.input_mode == "ptt", "round trip input_mode == ptt");
    remove(path);
}

int main() {
    auto logger = init_logger("test-barge-in-control", "warn");
    (void)logger;
    try {
        test_hard_stop_clears();
        test_played_ratio();
        test_fade_out_actually_fades();
        test_fade_out_clamped_to_remaining();
        test_no_push_during_fade();
        test_duck_lowers_gain_without_stopping();
        test_gain_clamped();
        test_start_playback_resets_gain();
        test_fade_out_without_playback();
        test_barge_in_config_parsing();
        test_input_mode_fallback();
        test_yaml_group_edit();
        test_yaml_round_trip();
    } catch (const std::exception& e) {
        cout << "EXCEPTION: " << e.what() << endl;
        return 1;
    }
    if (g_failed == 0) {
        cout << "ALL DONE (barge-in control)" << endl;
        return 0;
    }
    cout << g_failed << " CHECK(S) FAILED" << endl;
    return 1;
}